/********************************************************************
 * MSFS MAP 电脑端
 * 版权所有 © 2025-present SunMutian
 * Email: sunmutian88@gmail.com
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <dwmapi.h>
#include <SimConnect.h>
#include <shellscalingapi.h>

#include <thread>
#include <string>
#include <sstream>
#include <random>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <iostream>
#include "./ThirdParty/qrcodegen/qrcodegen.hpp"
#include "resource.h"

#pragma comment(lib, "SimConnect.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shcore.lib")

#pragma warning(disable : 28251)

 // -------------------- 全局 --------------------
HANDLE hSimConnect = NULL;
std::atomic<SOCKET> clientSock{ INVALID_SOCKET };
HWND hMainWnd = NULL;
HICON hIconGlobal = NULL;
std::string pairCode;
std::atomic<bool> isPaired{ false };
std::atomic<bool> running{ true };
std::mutex sockMutex;

struct SIMDATA {
    double latitude;
    double longitude;
    double altitude;
    double heading;
    double pitch;
    double roll;
    double gpsGroundSpeed;
    double indicatedAirspeed;
};

SIMDATA g_data = { 0 };
bool g_darkMode = false;
bool g_simConnected = false;
std::vector<std::vector<bool>> qrMatrix;

// 心跳
const int HEARTBEAT_INTERVAL_MS = 2000;
const int HEARTBEAT_TIMEOUT_MS = 6000;
std::atomic<long long> lastClientHeardMs{ 0 };

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// -------------------- 工具 --------------------
void EnableHighDPI() {
    HMODULE hShcore = LoadLibraryA("Shcore.dll");
    if (hShcore) {
        typedef HRESULT(WINAPI* SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);
        SetProcessDpiAwareness_t func = (SetProcessDpiAwareness_t)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (func) func(PROCESS_PER_MONITOR_DPI_AWARE);
        FreeLibrary(hShcore);
    }
    SetProcessDPIAware();
}

std::string GeneratePairCode() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dis(0, 9);
    std::stringstream ss;
    for (int i = 0; i < 6; i++) ss << dis(gen);
    return ss.str();
}

std::string GetLocalIP() {
    std::string ip = "127.0.0.1";
    char hostname[256] = { 0 };
    if (gethostname(hostname, sizeof(hostname)) != 0) return ip;

    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    if (getaddrinfo(hostname, NULL, &hints, &res) == 0) {
        for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
            struct sockaddr_in* addr = (struct sockaddr_in*)p->ai_addr;
            char buf[INET_ADDRSTRLEN] = { 0 };
            inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
            std::string cand = buf;
            if (cand.rfind("192.168.1.", 0) == 0) { ip = cand; break; }
            if (ip == "127.0.0.1") ip = cand;
        }
        freeaddrinfo(res);
    }
    return ip;
}

void UpdateWindowTitle() {
    std::string status = isPaired ? "已连接" : "未连接";
    std::string title = "MSFS MAP 电脑端 (" + status + ")";
    SetWindowTextA(hMainWnd, title.c_str());
}

void DetectSystemTheme() {
    DWORD value = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS) {
        g_darkMode = (value == 0);
    }
}

void GenerateQRCode(const std::string& text) {
    using namespace qrcodegen;
    QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
    int size = qr.getSize();
    qrMatrix.clear();
    qrMatrix.resize(size, std::vector<bool>(size, false));
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            qrMatrix[y][x] = qr.getModule(x, y);
}

// -------------------- SimConnect --------------------
void CALLBACK SimDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    if (!pData) return;
    switch (pData->dwID) {
    case SIMCONNECT_RECV_ID_SIMOBJECT_DATA: {
        SIMCONNECT_RECV_SIMOBJECT_DATA* obj = (SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;
        SIMDATA* d = (SIMDATA*)&obj->dwData;
        if (d) {
            g_data = *d;

            std::lock_guard<std::mutex> lock(sockMutex);
            SOCKET s = clientSock.load();
            if (isPaired && s != INVALID_SOCKET) {
                char msg[256];
                int written = sprintf_s(msg, sizeof(msg),
                    "%.6f,%.6f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f\n",
                    g_data.latitude, g_data.longitude, g_data.altitude,
                    g_data.heading, g_data.pitch, g_data.roll,
                    g_data.gpsGroundSpeed, g_data.indicatedAirspeed);
                if (written > 0) {
                    int res = send(s, msg, (int)strlen(msg), 0);
                    if (res == SOCKET_ERROR) {
                        closesocket(s);
                        clientSock = INVALID_SOCKET;
                        isPaired = false;
                        UpdateWindowTitle();
                    }
                    else lastClientHeardMs = NowMs();
                }
            }
        }
        InvalidateRect(hMainWnd, NULL, FALSE);
        break;
    }
    default: break;
    }
}

// -------------------- TCP 服务 --------------------
void TCPServerThread(int port) {
    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSock == INVALID_SOCKET) return;

    BOOL opt = TRUE;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons((u_short)port);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSock, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(serverSock); return;
    }
    if (listen(serverSock, 1) == SOCKET_ERROR) { closesocket(serverSock); return; }

    while (running) {
        SOCKET sock = accept(serverSock, NULL, NULL);
        if (!running) { if (sock != INVALID_SOCKET) closesocket(sock); break; }
        if (sock == INVALID_SOCKET) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }

        char buffer[64] = { 0 };
        int len = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (len > 0) {
            std::string recvCode(buffer, len);
            recvCode.erase(std::remove_if(recvCode.begin(), recvCode.end(), ::isspace), recvCode.end());
            if (recvCode == pairCode) {
                std::lock_guard<std::mutex> lock(sockMutex);
                SOCKET old = clientSock.load();
                if (old != INVALID_SOCKET) closesocket(old);
                clientSock = sock;
                isPaired = true;
                lastClientHeardMs = NowMs();
                UpdateWindowTitle();
                InvalidateRect(hMainWnd, NULL, TRUE);
            }
            else {
                std::string errorMsg = "ERR_PAIR_CODE";
                send(sock, errorMsg.c_str(), (int)errorMsg.length(), 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等待消息发送
                closesocket(sock);
            }
        }
        else closesocket(sock);
    }
    closesocket(serverSock);
}

// -------------------- 心跳 --------------------
void HeartbeatThread() {
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(HEARTBEAT_INTERVAL_MS));
        if (!isPaired) continue;

        std::lock_guard<std::mutex> lock(sockMutex);
        SOCKET s = clientSock.load();
        if (s == INVALID_SOCKET) { isPaired = false; UpdateWindowTitle(); continue; }

        const char* hb = "HEARTBEAT\n";
        int res = send(s, hb, (int)strlen(hb), 0);
        if (res == SOCKET_ERROR || NowMs() - lastClientHeardMs > HEARTBEAT_TIMEOUT_MS) {
            closesocket(s);
            clientSock = INVALID_SOCKET;
            isPaired = false;
            UpdateWindowTitle();
        }
        else lastClientHeardMs = NowMs();
    }
}

// -------------------- SimConnect 管理 --------------------
void SimConnectManagerThread() {
    while (running) {
        if (!hSimConnect) {
            HRESULT hr = SimConnect_Open(&hSimConnect, "MSFS Sender", NULL, 0, 0, 0);
            if (SUCCEEDED(hr)) {
                g_simConnected = true;
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE LATITUDE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE LONGITUDE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE ALTITUDE", "feet");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE HEADING DEGREES TRUE", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE PITCH DEGREES", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "PLANE BANK DEGREES", "degrees");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "GPS GROUND SPEED", "meters per second");
                SimConnect_AddToDataDefinition(hSimConnect, 0, "AIRSPEED INDICATED", "meters per second");

                SimConnect_RequestDataOnSimObject(hSimConnect, 0, 0, SIMCONNECT_OBJECT_ID_USER, SIMCONNECT_PERIOD_SIM_FRAME);
                PostMessage(hMainWnd, WM_APP + 1, 0, 0);
            }
            else g_simConnected = false;
        }

        if (hSimConnect) {
            HRESULT hr = SimConnect_CallDispatch(hSimConnect, SimDispatch, NULL);
            if (FAILED(hr)) {
                SimConnect_Close(hSimConnect);
                hSimConnect = NULL;
                g_simConnected = false;
                PostMessage(hMainWnd, WM_APP + 2, 0, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (hSimConnect) { SimConnect_Close(hSimConnect); hSimConnect = NULL; g_simConnected = false; }
}

// -------------------- 窗口 --------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool btnMinHover = false, btnCloseHover = false;
    static bool mouseTrackActive = false;

    switch (msg) {
    case WM_CREATE: {
        hMainWnd = hwnd;
        DetectSystemTheme();
        pairCode = GeneratePairCode();
        std::string ip = GetLocalIP();
        GenerateQRCode(ip + ":5000;PAIR:" + pairCode);

        std::thread(TCPServerThread, 5000).detach();
        std::thread(HeartbeatThread).detach();
        std::thread(SimConnectManagerThread).detach();
        break;
    }

    case WM_APP + 1: case WM_APP + 2:
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    case WM_APP + 100:
    {
        if (!GetProp(hwnd, L"ShownSimErrorFlag")) {
            std::string msg = "未检测到 MSFS SimConnect Api 连接。请启动 Microsoft Flight Simulator 游戏后再试!\n程序即将退出...";
            MessageBoxA(hwnd, msg.c_str(), "ERR_MSFS_API", MB_OK | MB_ICONERROR);
            SetProp(hwnd, L"ShownSimErrorFlag", (HANDLE)1);

            // 点击确认后退出程序
            running = false;

            // 关闭 Socket
            {
                std::lock_guard<std::mutex> lock(sockMutex);
                SOCKET s = clientSock.load();
                if (s != INVALID_SOCKET) {
                    closesocket(s);
                    clientSock = INVALID_SOCKET;
                }
            }

            // 触发窗口关闭，安全退出消息循环
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    }
    case WM_APP + 101:
    {
        // 连接成功时清除标志，让将来断开时能再次弹窗
        if (GetProp(hwnd, L"ShownSimErrorFlag")) RemoveProp(hwnd, L"ShownSimErrorFlag");
        break;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT rect;
        GetClientRect(hwnd, &rect);

        // -------------------- 双缓冲 --------------------
        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // 背景颜色
        COLORREF bgColor = g_darkMode ? RGB(30, 30, 30) : RGB(255, 255, 255);
        HBRUSH hBrushBG = CreateSolidBrush(bgColor);
        FillRect(memDC, &rect, hBrushBG);
        DeleteObject(hBrushBG);

        // -------------------- 自绘标题栏 --------------------
        int titleBarHeight = 30;
        RECT titleRect = { 0, 0, rect.right, titleBarHeight };
        HBRUSH hBrushTitle = CreateSolidBrush(g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180));
        FillRect(memDC, &titleRect, hBrushTitle);
        DeleteObject(hBrushTitle);

        // 绘制图标（小图标）
        HICON hIconSmall = (HICON)SendMessage(hwnd, WM_GETICON, ICON_SMALL, 0);
        if (!hIconSmall) hIconSmall = hIconGlobal;
        if (hIconSmall) {
            // 绘制 16x16 小图标，距左 6px，垂直居中
            DrawIconEx(memDC, 6, (titleBarHeight - 16) / 2, hIconSmall, 16, 16, 0, NULL, DI_NORMAL);
        }

        // 标题文字
        char wndTitle[256] = { 0 };
        GetWindowTextA(hwnd, wndTitle, sizeof(wndTitle));
        if (!hSimConnect) {
            strncpy_s(wndTitle, sizeof(wndTitle), "MSFS MAP 电脑端 (无 MSFS SimConnect Api 连接)", _TRUNCATE);
            wndTitle[sizeof(wndTitle) - 1] = '\0';
        }
        // 使用印刷体字体
        HFONT hFontTitle = CreateFont(
            16, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑"
        );
        HFONT oldFont = (HFONT)SelectObject(memDC, hFontTitle);

        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));

        RECT textRect = { 26, 0, rect.right - 100, titleBarHeight }; // 留出图标和按钮空间
        DrawTextA(memDC, wndTitle, -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_LEFT);

        SelectObject(memDC, oldFont);
        DeleteObject(hFontTitle);

        // -------------------- 最小化和关闭按钮 --------------------
        RECT btnMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT btnClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        // 自动适配黑白模式
        COLORREF minColor = g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180);
        COLORREF minHoverColor = g_darkMode ? RGB(90, 90, 90) : RGB(150, 150, 150);
        COLORREF closeColor = g_darkMode ? RGB(45, 45, 45) : RGB(180, 180, 180);
        COLORREF closeHoverColor = g_darkMode ? RGB(255, 0, 0) : RGB(255, 0, 0);

        HBRUSH hBtnMin = CreateSolidBrush(btnMinHover ? minHoverColor : minColor);
        HBRUSH hBtnClose = CreateSolidBrush(btnCloseHover ? closeHoverColor : closeColor);
        FillRect(memDC, &btnMin, hBtnMin);
        FillRect(memDC, &btnClose, hBtnClose);
        DeleteObject(hBtnMin);
        DeleteObject(hBtnClose);

        // 按钮文字
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, g_darkMode ? RGB(255, 255, 255) : RGB(0, 0, 0)); // 文字黑白切换
        DrawTextA(memDC, "–", -1, &btnMin, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextA(memDC, "X", -1, &btnClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);


        // -------------------- 内容区 --------------------
        RECT contentRect = { 0, titleBarHeight, rect.right, rect.bottom };

        if (!hSimConnect)
        {
            // 绘制提示文本
            std::string msg = "未检测到 MSFS SimConnect Api 连接。请启动 Microsoft Flight Simulator 游戏后再试!";
            HFONT hFont = CreateFont(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT oldFont2 = (HFONT)SelectObject(memDC, hFont);
            SetWindowTextA(hMainWnd, "MSFS MAP 电脑端 (无 MSFS SimConnect Api 连接)");
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));
            DrawTextA(memDC, msg.c_str(), -1, &contentRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

            SelectObject(memDC, oldFont2);
            DeleteObject(hFont);

            // 通过 PostMessage 请求主线程安全弹窗（只在未弹出过时请求）
            if (!GetProp(hwnd, L"ShownSimErrorFlag")) {
                PostMessage(hwnd, WM_APP + 100, 0, 0);
            }
        }
        else
        {
            // -------------------- 二维码和右侧信息居中 --------------------
            int qrSize = (int)qrMatrix.size();
            int scale = 8;
            int spacing = 20;
            int noteHeight = 50;
            int noteSpacing = 8;
            int rightWidth = 300;

            int margin = 2; // 二维码外围白边（模块数）
            int modulePx = qrSize * scale;              // 实际模块区像素
            int quietPx = margin * scale * 2;           // quiet zone 两侧总像素
            int qrWithQuietPx = modulePx + quietPx;    // 模块 + quiet zone 总像素

            // 在白色卡片上额外给一点 padding（使白卡片比二维码略大，便于圆角视觉）
            int whitePad = scale * 2; // you can tune this (pixels)

            int whiteBgWidth = qrWithQuietPx + whitePad * 2;
            int whiteBgHeight = qrWithQuietPx + whitePad * 2;

            int totalWidth = whiteBgWidth + spacing + rightWidth;
            int totalHeight = whiteBgHeight + noteHeight + noteSpacing;

            // 把内容区的起始 Y 从 titleBarHeight 开始，并在剩余区域内居中
            int contentAreaHeight = rect.bottom - titleBarHeight;
            int leftMargin = (rect.right - totalWidth) / 2;
            int topMargin = titleBarHeight + (contentAreaHeight - totalHeight) / 2;
            if (topMargin < titleBarHeight) topMargin = titleBarHeight + 8;

            // -------------------- 白色圆角背景（卡片） --------------------
            int bgRadius = 20;
            HRGN qrBgRgn = CreateRoundRectRgn(
                leftMargin,
                topMargin,
                leftMargin + whiteBgWidth,
                topMargin + whiteBgHeight,
                bgRadius, bgRadius
            );
            HBRUSH hBrushWhite = CreateSolidBrush(g_darkMode ? RGB(245, 245, 245) : RGB(255, 255, 255));
            FillRgn(memDC, qrBgRgn, hBrushWhite);
            DeleteObject(qrBgRgn);

            // -------------------- 在白色区域内居中绘制二维码 --------------------
            int drawOriginX = leftMargin + whitePad + margin * scale;
            int drawOriginY = topMargin + whitePad + margin * scale;

            HBRUSH hBrushFG = CreateSolidBrush(RGB(0, 0, 0));     // 黑
            HBRUSH hBrushBGQR = CreateSolidBrush(RGB(255, 255, 255)); // 白 (模块背景)

            for (int y = 0; y < qrSize; y++)
            {
                for (int x = 0; x < qrSize; x++)
                {
                    RECT r = {
                        drawOriginX + x * scale,
                        drawOriginY + y * scale,
                        drawOriginX + (x + 1) * scale,
                        drawOriginY + (y + 1) * scale
                    };
                    FillRect(memDC, &r, qrMatrix[y][x] ? hBrushFG : hBrushBGQR);
                }
            }

            // 清理二维码用画刷
            DeleteObject(hBrushFG);
            DeleteObject(hBrushBGQR);
            DeleteObject(hBrushWhite);

            // -------------------- 二维码下方注释 --------------------
            std::stringstream note;
            note << "通过MSFS Map手机端连接\n"
                << "(需要在同一个局域网内)\n"
                << "IP: " << GetLocalIP() << " 端口: 5000\n"
                << "配对码: " << pairCode;

            HFONT hFontNote = CreateFont(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT oldFontNote = (HFONT)SelectObject(memDC, hFontNote);

            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));

            RECT noteRect = {
                leftMargin,
                topMargin + whiteBgHeight + noteSpacing,
                leftMargin + whiteBgWidth,
                topMargin + whiteBgHeight + noteSpacing + noteHeight * 2
            };
            DrawTextA(memDC, note.str().c_str(), -1, &noteRect, DT_CENTER | DT_TOP | DT_WORDBREAK);

            SelectObject(memDC, oldFontNote);
            DeleteObject(hFontNote);

            // -------------------- 右侧信息栏 --------------------
            int dataLeft = leftMargin + whiteBgWidth + spacing;
            int dataRight = dataLeft + rightWidth;
            int dataTop = topMargin;
            int dataBottom = topMargin + whiteBgHeight + noteHeight + noteSpacing;

            COLORREF dataBgColor = g_darkMode ? RGB(50, 50, 50) : RGB(200, 200, 200);
            HBRUSH hBrushData = CreateSolidBrush(dataBgColor);
            HRGN hRgn = CreateRoundRectRgn(dataLeft, dataTop, dataRight, dataBottom, 40, 40);
            FillRgn(memDC, hRgn, hBrushData);
            DeleteObject(hBrushData);
            DeleteObject(hRgn);

            // -------------------- 右侧内容 --------------------
            HFONT hFontStatus = CreateFont(24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT hFontAPI = CreateFont(24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
            HFONT hFontData = CreateFont(20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");

            // 文本内容
            std::string statusText = isPaired ? "已连接手机" : "未连接手机";
            std::string statusAPI = "MSFS SimConnect API: 已连接";

            std::stringstream coordSS;
            coordSS << "当前游戏数据\n";
            coordSS << "纬度(Lat): " << abs(g_data.latitude) << (g_data.latitude >= 0 ? " N" : " S") << "\n";
            coordSS << "经度(Lon): " << abs(g_data.longitude) << (g_data.longitude >= 0 ? " E" : " W") << "\n";
            coordSS << "高度(Alt): " << g_data.altitude << " 英尺 / " << g_data.altitude * 0.3048 << " 米\n";
            coordSS << "航向(Heading): " << g_data.heading << "°\n";
            coordSS << "俯仰(Pitch): " << g_data.pitch << "°\n";
            coordSS << "横滚(Roll): " << g_data.roll << "°\n";
            coordSS << "地速(GS): " << g_data.gpsGroundSpeed << " m/s\n";
            coordSS << "空速(IAS): " << g_data.indicatedAirspeed << " m/s\n";
            std::string coordText = coordSS.str();

            // 可用宽度（去掉左右 padding）
            int paddingX = 16;
            int contentWidth = (dataRight - dataLeft) - paddingX * 2;

            // 计算每块高度
            RECT tmp = { 0,0, contentWidth, 0 };

            // 状态高度
            SelectObject(memDC, hFontStatus);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, statusText.c_str(), -1, &tmp, DT_CALCRECT | DT_CENTER | DT_SINGLELINE);
            int hStatus = tmp.bottom - tmp.top;

            // API 高度
            SelectObject(memDC, hFontAPI);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, statusAPI.c_str(), -1, &tmp, DT_CALCRECT | DT_CENTER | DT_SINGLELINE);
            int hAPI = tmp.bottom - tmp.top;

            // Data 高度 (多行，换行并以居中显示)
            SelectObject(memDC, hFontData);
            tmp.left = 0; tmp.top = 0; tmp.right = contentWidth; tmp.bottom = 0;
            DrawTextA(memDC, coordText.c_str(), -1, &tmp, DT_CALCRECT | DT_WORDBREAK | DT_CENTER);
            int hData = tmp.bottom - tmp.top;

            int gap = 8; // 各块之间间隔
            int totalContentHeight = hStatus + gap + hAPI + gap + hData;

            // 起始 Y，使内容垂直居中在卡片内
            int boxInnerTop = dataTop + 16; // 小外边距
            int boxInnerBottom = dataBottom - 16;
            int boxInnerHeight = boxInnerBottom - boxInnerTop;
            int startY = boxInnerTop + (boxInnerHeight - totalContentHeight) / 2;
            if (startY < boxInnerTop) startY = boxInnerTop;

            // 绘制状态（居中）
            RECT drawRect = { dataLeft + paddingX, startY, dataRight - paddingX, startY + hStatus };
            SelectObject(memDC, hFontStatus);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, isPaired ? RGB(0, 200, 0) : RGB(200, 0, 0));
            DrawTextA(memDC, statusText.c_str(), -1, &drawRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 绘制 API 状态
            int y2 = drawRect.bottom + gap;
            RECT drawRectAPI = { dataLeft + paddingX, y2, dataRight - paddingX, y2 + hAPI };
            SelectObject(memDC, hFontAPI);
            SetTextColor(memDC, RGB(0, 200, 0));
            DrawTextA(memDC, statusAPI.c_str(), -1, &drawRectAPI, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // 绘制数据（多行）
            int y3 = drawRectAPI.bottom + gap;
            RECT drawRectData = { dataLeft + paddingX, y3, dataRight - paddingX, y3 + hData };
            SelectObject(memDC, hFontData);
            SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(0, 0, 0));
            DrawTextA(memDC, coordText.c_str(), -1, &drawRectData, DT_CENTER | DT_WORDBREAK);

            // 清理字体对象
            SelectObject(memDC, GetStockObject(SYSTEM_FONT));
            DeleteObject(hFontStatus);
            DeleteObject(hFontAPI);
            DeleteObject(hFontData);
        }

        // -------------------- 版权信息 --------------------
        HFONT hFontCopyright = CreateFont(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"微软雅黑");
        HFONT oldFontCopy = (HFONT)SelectObject(memDC, hFontCopyright);
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, g_darkMode ? RGB(180, 180, 180) : RGB(100, 100, 100));
        RECT textRectC = { rect.right - 500, rect.bottom - 30, rect.right - 10, rect.bottom - 10 };
        DrawTextA(memDC, "(C) Copyright 2025–present SunMutian 版权所有 - Email: sunmutian88@gmail.com", -1, &textRectC, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
        SelectObject(memDC, oldFontCopy);
        DeleteObject(hFontCopyright);

        // -------------------- 绘制边框 --------------------
        int borderRadius = 20;
        HPEN hPenBorder = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(25, 25, 25) : RGB(250, 250, 250));
        HGDIOBJ oldPen = SelectObject(memDC, hPenBorder);
        HBRUSH hOldBrush = (HBRUSH)SelectObject(memDC, GetStockObject(NULL_BRUSH)); // 不填充

        // 使用 RoundRect 绘制圆角边框
        Rectangle(memDC, 0, 0, rect.right, rect.bottom);
        RoundRect(memDC, 0, 0, rect.right, rect.bottom, borderRadius, borderRadius);

        SelectObject(memDC, hOldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(hPenBorder);

        // 已连接 -> 清除弹窗标志（允许将来断开时再次弹窗）
        PostMessage(hwnd, WM_APP + 101, 0, 0);
        // -------------------- 显示到窗口 --------------------
        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);

        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(hwnd, &ps);
        break;
    }

    case WM_LBUTTONDOWN:
    {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleBarHeight = 30;

        RECT btnMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT btnClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        if (PtInRect(&btnClose, pt)) { PostMessage(hwnd, WM_CLOSE, 0, 0); }
        else if (PtInRect(&btnMin, pt)) { ShowWindow(hwnd, SW_MINIMIZE); }
        else if (pt.y <= titleBarHeight) {
            ReleaseCapture();
            SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        }
        break;
    }

    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleBarHeight = 30;
        RECT rMin = { rect.right - 80, 0, rect.right - 40, titleBarHeight };
        RECT rClose = { rect.right - 40, 0, rect.right, titleBarHeight };

        bool prevMin = btnMinHover;
        bool prevClose = btnCloseHover;

        btnMinHover = PtInRect(&rMin, pt);
        btnCloseHover = PtInRect(&rClose, pt);

        if (!mouseTrackActive) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            mouseTrackActive = true;
        }

        if (btnMinHover != prevMin || btnCloseHover != prevClose) {
            RECT rcInvalidate = { rect.right - 80, 0, rect.right, titleBarHeight };
            InvalidateRect(hwnd, &rcInvalidate, FALSE);
        }
    }
                     break;

    case WM_MOUSELEAVE:
        btnMinHover = false;
        btnCloseHover = false;
        mouseTrackActive = false;
        {
            RECT rect; GetClientRect(hwnd, &rect);
            int titleBarHeight = 30;
            RECT rcInvalidate = { rect.right - 80, 0, rect.right, titleBarHeight };
            InvalidateRect(hwnd, &rcInvalidate, FALSE);
        }
        break;

    case WM_DESTROY:
        running = false;
        {
            std::lock_guard<std::mutex> lock(sockMutex);
            SOCKET s = clientSock.load(); if (s != INVALID_SOCKET) { closesocket(s); clientSock = INVALID_SOCKET; }
        }
        PostQuitMessage(0);
        break;

    default:
        break;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// -------------------- 主入口 --------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    EnableHighDPI();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { MessageBoxA(NULL, "WSAStartup failed", "Error", MB_OK | MB_ICONERROR); return -1; }

    hIconGlobal = (HICON)LoadIcon(hInstance, MAKEINTRESOURCE(IDI_ICON1));

    WNDCLASSW wc = {};
    wc.lpszClassName = L"MSFSMapWin";
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIconGlobal;
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        0, L"MSFSMapWin", L"MSFS MAP 电脑端 (未连接)",
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
        200, 200, 820, 460, NULL, NULL, hInstance, NULL
    );
    // 设置窗口圆角
    int radius = 20; // 圆角半径
    RECT rc;
    GetClientRect(hwnd, &rc);
    HRGN hRgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, radius, radius);
    SetWindowRgn(hwnd, hRgn, TRUE);
    if (!hwnd) { WSACleanup(); return -1; }

    // 确保小图标可用（WM_GETICON / WM_SETICON）
    if (hIconGlobal) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconGlobal);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconGlobal);
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }

    WSACleanup();
    if (hIconGlobal) DestroyIcon(hIconGlobal);
    return (int)msg.wParam;
}
