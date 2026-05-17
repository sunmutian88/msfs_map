/**********************************﻿/********************************************************************
 * MSFS MAP 电脑端 （Web版本）
 * 版权所有 © 2025-present SunMutian
 * Email: sunmutian88@gmail.com
 * 时间: 2026-5-17
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
#include <iphlpapi.h>
#include "resource.h"

#include <iostream>
#include <thread>
#include <string>
#include <sstream>
#include <random>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <algorithm>
#include <cstdio>

#pragma comment(lib, "SimConnect.lib")
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "Shcore.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "msimg32.lib")
#pragma warning(disable : 28251)

 // -------------------- 全局 --------------------
HANDLE hSimConnect = NULL;
HWND hMainWnd = NULL;
HICON hIconGlobal = NULL;
std::atomic<bool> running{ true };
int httpPort = 8080;

struct SIMDATA {
    double latitude, longitude, altitude, heading, pitch, roll, gpsGroundSpeed, indicatedAirspeed;
};
SIMDATA g_data = { 0 };
bool g_darkMode = false;
bool g_simConnected = false;

std::atomic<bool> httpRunning{ true };
std::string latestJsonData;
std::mutex jsonMutex;

// 语言
enum class AppLanguage { Chinese, English };
AppLanguage g_language = AppLanguage::English;

// 字符串资源
std::wstring GetAppTitle() {
    return g_language == AppLanguage::Chinese ? L"MSFS 飞行地图 (Web版)" : L"MSFS Flight Map (Web Version)";
}
std::wstring GetMainPrompt() {
    return g_language == AppLanguage::Chinese ? L"请在浏览器中打开以下地址" : L"Please open the following address in your browser";
}
std::wstring GetSimStatusText(bool connected) {
    if (connected)
        return g_language == AppLanguage::Chinese ? L"SimConnect: 已连接" : L"SimConnect: Connected";
    else
        return g_language == AppLanguage::Chinese ? L"SimConnect: 未连接 (请启动 MSFS)" : L"SimConnect: Not connected (MSFS not running)";
}

// -------------------- 工具函数 --------------------
void EnableHighDPI() {
    HMODULE hShcore = LoadLibraryA("Shcore.dll");
    if (hShcore) {
        typedef HRESULT(WINAPI* SetProcessDpiAwareness_t)(PROCESS_DPI_AWARENESS);
        auto func = (SetProcessDpiAwareness_t)GetProcAddress(hShcore, "SetProcessDpiAwareness");
        if (func) func(PROCESS_PER_MONITOR_DPI_AWARE);
        FreeLibrary(hShcore);
    }
    SetProcessDPIAware();
}

void DetectSystemLanguage() {
    LANGID langId = GetUserDefaultUILanguage();
    WORD primaryLang = PRIMARYLANGID(langId);
    g_language = (primaryLang == LANG_CHINESE) ? AppLanguage::Chinese : AppLanguage::English;
}

std::string GetBestLocalIP() {
    std::string bestIp = "127.0.0.1";
    ULONG bufSize = 15000;
    std::vector<BYTE> buffer(bufSize);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());

    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &bufSize);

    // 1. 核心修正：处理缓冲区溢出，重新分配内存
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bufSize);
        pAddresses = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &bufSize);
    }

    if (result != NO_ERROR) {
        return bestIp;
    }

    int bestScore = -1; // 用于网卡优先级评分

    for (PIP_ADAPTER_ADDRESSES p = pAddresses; p; p = p->Next) {
        // 2. 基本状态过滤
        if (p->OperStatus != IfOperStatusUp) continue;
        if (p->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        // 3. 增强过滤：同时通过内部特征和名字排除虚拟网卡
        std::wstring name = p->FriendlyName ? p->FriendlyName : L"";
        std::wstring desc = p->Description ? p->Description : L"";

        // 排除常见的虚拟网卡特征（支持多语言模糊匹配描述）
        if (name.find(L"Virtual") != std::wstring::npos || desc.find(L"Virtual") != std::wstring::npos ||
            name.find(L"VMware") != std::wstring::npos || desc.find(L"VMware") != std::wstring::npos ||
            name.find(L"VirtualBox") != std::wstring::npos || desc.find(L"VirtualBox") != std::wstring::npos ||
            name.find(L"vEthernet") != std::wstring::npos || desc.find(L"vEthernet") != std::wstring::npos) {
            continue;
        }

        // 4. 网卡类型优先级评分：以太网(3) > 无线Wi-Fi(2) > 其他(1)
        int currentScore = 1;
        if (p->IfType == IF_TYPE_ETHERNET_CSMACD) {
            currentScore = 3;
        }
        else if (p->IfType == IF_TYPE_IEEE80211) { // Wi-Fi
            currentScore = 2;
        }

        for (PIP_ADAPTER_UNICAST_ADDRESS u = p->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr) continue;

            sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            char ipStr[INET_ADDRSTRLEN] = { 0 };

            if (inet_ntop(AF_INET, &(addr->sin_addr), ipStr, sizeof(ipStr))) {
                std::string ip = ipStr;

                // 5. 过滤回环和链路本地地址 (169.254.x.x)
                if (ip.rfind("127.", 0) == 0 || ip.rfind("169.254.", 0) == 0) {
                    continue;
                }

                // 6. 如果当前网卡优先级更高，则更新最佳 IP
                if (currentScore > bestScore) {
                    bestScore = currentScore;
                    bestIp = ip;
                }
            }
        }
    }
    return bestIp;
}


void DetectSystemTheme() {
    DWORD value = 0, size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &value, &size) == ERROR_SUCCESS)
        g_darkMode = (value == 0);
}

// -------------------- SimConnect --------------------
void CALLBACK SimDispatch(SIMCONNECT_RECV* pData, DWORD cbData, void* pContext) {
    if (!pData) return;
    if (pData->dwID == SIMCONNECT_RECV_ID_SIMOBJECT_DATA) {
        auto obj = (SIMCONNECT_RECV_SIMOBJECT_DATA*)pData;
        SIMDATA* d = (SIMDATA*)&obj->dwData;
        if (d) {
            g_data = *d;
            std::lock_guard<std::mutex> lock(jsonMutex);
            char buf[512];
            snprintf(buf, sizeof(buf),
                "{\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"heading\":%.2f,"
                "\"pitch\":%.2f,\"roll\":%.2f,\"gs\":%.2f,\"ias\":%.2f}",
                g_data.latitude, g_data.longitude, g_data.altitude, g_data.heading,
                g_data.pitch, g_data.roll, g_data.gpsGroundSpeed, g_data.indicatedAirspeed);
            latestJsonData = buf;
        }
        InvalidateRect(hMainWnd, NULL, FALSE);
    }
}

// -------------------- 内嵌网页 (单HTML，JS国际化) --------------------
std::string GetEmbeddedHtmlPage() {
    return std::string() +
        "<!DOCTYPE html>\n"
        "<html>\n"
        "<head>\n"
        "    <meta charset=\"UTF-8\">\n"
        "    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, user-scalable=no\">\n"
        "    <title>MSFS Live Map</title>\n"
        "    <link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\" />\n"
        "    <script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>\n"
        "    <style>\n"
        "        body { margin:0; padding:0; font-family: 'Segoe UI', Roboto, sans-serif; }\n"
        "        #map { height: 100vh; width: 100%; }\n"
        "        .info-panel {\n"
        "            position: fixed; bottom: 20px; left: 20px; right: 20px;\n"
        "            background: rgba(0,0,0,0.75); backdrop-filter: blur(10px); border-radius: 20px;\n"
        "            padding: 12px 16px; color: white; font-size: 14px; z-index: 1000;\n"
        "            pointer-events: none; box-shadow: 0 2px 10px rgba(0,0,0,0.3);\n"
        "            border: 1px solid rgba(255,255,255,0.2);\n"
        "        }\n"
        "        .status {\n"
        "            position: fixed; top: 20px; right: 20px;\n"
        "            background: rgba(0,0,0,0.6); padding: 6px 12px; border-radius: 20px;\n"
        "            font-size: 12px; color: #0f0; z-index: 1000; font-family: monospace;\n"
        "            pointer-events: none;\n"
        "        }\n"
        "        .follow-btn {\n"
        "            position: fixed; bottom: 120px; right: 20px;\n"
        "            background: rgba(0,0,0,0.7); padding: 8px 16px; border-radius: 30px;\n"
        "            color: white; font-size: 14px; z-index: 1000; cursor: pointer;\n"
        "            border: none; font-weight: bold; backdrop-filter: blur(5px);\n"
        "        }\n"
        "        @media (max-width: 600px) {\n"
        "            .info-panel { font-size: 12px; padding: 8px 12px; }\n"
        "            .status { font-size: 10px; top: 12px; right: 12px; }\n"
        "            .follow-btn { bottom: 100px; right: 12px; padding: 6px 12px; font-size: 12px; }\n"
        "        }\n"
        "        .aircraft-icon { background: transparent; border: none; font-size: 28px; text-align: center; line-height: 28px; }\n"
        "    </style>\n"
        "</head>\n"
        "<body>\n"
        "<div id=\"map\"></div>\n"
        "<div class=\"status\" id=\"status\">--</div>\n"
        "<div class=\"info-panel\">\n"
        "    <div><span id=\"labelAircraft\">✈️ Aircraft:</span> <span id=\"aircraftCoord\">--</span></div>\n"
        "    <div><span id=\"labelLocation\">📍 Location:</span> <span id=\"address\">--</span></div>\n"
        "    <div><span id=\"labelAltitude\">📊 Altitude:</span> <span id=\"altitude\">--</span> <span id=\"unitAlt\">ft</span> &nbsp;| <span id=\"labelHeading\">Heading:</span> <span id=\"heading\">--</span>° &nbsp;| <span id=\"labelGS\">GS:</span> <span id=\"gs\">--</span> m/s</div>\n"
        "</div>\n"
        "<button class=\"follow-btn\" id=\"followBtn\">🔍 Follow Aircraft</button>\n"
        "<script>\n"
        "    // 国际化文本资源\n"
        "    const i18n = {\n"
        "        zh: {\n"
        "            aircraft: '✈️ 飞机位置:',\n"
        "            location: '📍 地址:',\n"
        "            altitude: '📊 高度:',\n"
        "            heading: '航向:',\n"
        "            gs: '地速:',\n"
        "            unitAlt: '英尺',\n"
        "            statusLive: '✈️ 实时',\n"
        "            statusWait: '⏳ 等待飞机数据...',\n"
        "            statusLost: '⚠️ 连接断开',\n"
        "            follow: '🔍 跟随飞机',\n"
        "            following: '🔍 跟随中 (点击取消)',\n"
        "            addressUnknown: '未知'\n"
        "        },\n"
        "        en: {\n"
        "            aircraft: '✈️ Aircraft:',\n"
        "            location: '📍 Location:',\n"
        "            altitude: '📊 Altitude:',\n"
        "            heading: 'Heading:',\n"
        "            gs: 'GS:',\n"
        "            unitAlt: 'ft',\n"
        "            statusLive: '✈️ Live',\n"
        "            statusWait: '⏳ Waiting for aircraft...',\n"
        "            statusLost: '⚠️ Connection lost',\n"
        "            follow: '🔍 Follow Aircraft',\n"
        "            following: '🔍 Following (click to cancel)',\n"
        "            addressUnknown: 'Unknown'\n"
        "        }\n"
        "    };\n"
        "    let lang = navigator.language.toLowerCase().startsWith('zh') ? 'zh' : 'en';\n"
        "    function t(key) { return i18n[lang][key]; }\n"
        "    // 更新所有静态文本\n"
        "    document.getElementById('labelAircraft').innerText = t('aircraft');\n"
        "    document.getElementById('labelLocation').innerText = t('location');\n"
        "    document.getElementById('labelAltitude').innerText = t('altitude');\n"
        "    document.getElementById('labelHeading').innerText = t('heading');\n"
        "    document.getElementById('labelGS').innerText = t('gs');\n"
        "    document.getElementById('unitAlt').innerText = t('unitAlt');\n"
        "    let followBtn = document.getElementById('followBtn');\n"
        "    followBtn.innerText = t('follow');\n"
        "\n"
        "    let map = L.map('map').setView([0, 0], 4);\n"
        "    L.tileLayer('https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png', { attribution: '&copy; <a href=\"https://www.openstreetmap.org/copyright\">OSM</a> & CartoDB' }).addTo(map);\n"
        "    let aircraftMarker = null, aircraftLat = 0, aircraftLon = 0, followEnabled = false;\n"
        "    function getAircraftIcon(heading) {\n"
        "        return L.divIcon({ html: '<div style=\"transform: rotate(' + heading + 'deg); font-size: 28px;\">✈️</div>', iconSize: [28,28], className: 'aircraft-icon' });\n"
        "    }\n"
        "    let lastGeocodeTime = 0;\n"
        "    function reverseGeocode(lat, lon) {\n"
        "        const now = Date.now();\n"
        "        if (now - lastGeocodeTime < 2000) return;\n"
        "        lastGeocodeTime = now;\n"
        "        fetch(`https://nominatim.openstreetmap.org/reverse?format=json&lat=${lat}&lon=${lon}&zoom=18&addressdetails=1`)\n"
        "            .then(res => res.json())\n"
        "            .then(data => {\n"
        "                const addr = data.address;\n"
        "                const country = addr.country || '';\n"
        "                const state = addr.state || addr.province || '';\n"
        "                const city = addr.city || addr.town || addr.village || '';\n"
        "                const parts = [country, state, city].filter(p => p);\n"
        "                document.getElementById('address').innerText = parts.join(', ') || t('addressUnknown');\n"
        "            })\n"
        "            .catch(e => console.warn(e));\n"
        "    }\n"
        "    function fetchData() {\n"
        "        fetch('/api/data')\n"
        "            .then(res => res.json())\n"
        "            .then(data => {\n"
        "                if (data.lat && data.lon) {\n"
        "                    aircraftLat = data.lat; aircraftLon = data.lon;\n"
        "                    document.getElementById('aircraftCoord').innerText = `${data.lat.toFixed(4)}°, ${data.lon.toFixed(4)}°`;\n"
        "                    document.getElementById('altitude').innerText = data.alt.toFixed(0);\n"
        "                    document.getElementById('heading').innerText = data.heading.toFixed(0);\n"
        "                    document.getElementById('gs').innerText = data.gs.toFixed(1);\n"
        "                    document.getElementById('status').innerHTML = t('statusLive');\n"
        "                    document.getElementById('status').style.color = '#0f0';\n"
        "                    if (!aircraftMarker) aircraftMarker = L.marker([aircraftLat, aircraftLon], { icon: getAircraftIcon(data.heading) }).addTo(map);\n"
        "                    else { aircraftMarker.setLatLng([aircraftLat, aircraftLon]); aircraftMarker.setIcon(getAircraftIcon(data.heading)); }\n"
        "                    if (followEnabled) map.setView([aircraftLat, aircraftLon], map.getZoom());\n"
        "                    reverseGeocode(aircraftLat, aircraftLon);\n"
        "                } else {\n"
        "                    document.getElementById('status').innerHTML = t('statusWait');\n"
        "                }\n"
        "            })\n"
        "            .catch(err => {\n"
        "                console.warn(err);\n"
        "                document.getElementById('status').innerHTML = t('statusLost');\n"
        "                document.getElementById('status').style.color = '#f00';\n"
        "            });\n"
        "    }\n"
        "    followBtn.addEventListener('click', () => {\n"
        "        followEnabled = !followEnabled;\n"
        "        followBtn.innerText = followEnabled ? t('following') : t('follow');\n"
        "        if (followEnabled && aircraftLat && aircraftLon) map.setView([aircraftLat, aircraftLon], map.getZoom());\n"
        "    });\n"
        "    setInterval(fetchData, 500);\n"
        "    fetchData();\n"
        "</script>\n"
        "</body>\n"
        "</html>";
}

// -------------------- HTTP 服务器 --------------------
void HandleHttpRequest(SOCKET client, const std::string& request) {
    std::istringstream reqStream(request);
    std::string method, path, version;
    reqStream >> method >> path >> version;
    std::string response, contentType;
    if (path == "/" || path == "/index.html") {
        response = GetEmbeddedHtmlPage();
        contentType = "text/html; charset=utf-8";
    }
    else if (path == "/api/data") {
        std::lock_guard<std::mutex> lock(jsonMutex);
        response = latestJsonData.empty() ? "{}" : latestJsonData;
        contentType = "application/json";
    }
    else {
        response = "<html><body><h1>404 Not Found</h1></body></html>";
        contentType = "text/html; charset=utf-8";
    }
    std::stringstream resp;
    resp << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Content-Length: " << response.size() << "\r\n"
        << "Connection: close\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        << "\r\n"
        << response;
    send(client, resp.str().c_str(), (int)resp.str().size(), 0);
}

void HTTPServerThread(int port) {
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) return;
    BOOL opt = TRUE;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR || listen(listenSock, 5) == SOCKET_ERROR) {
        closesocket(listenSock); return;
    }
    while (httpRunning) {
        fd_set fdset; FD_ZERO(&fdset); FD_SET(listenSock, &fdset);
        timeval tv = { 1, 0 };
        if (select(0, &fdset, NULL, NULL, &tv) <= 0) continue;
        SOCKET client = accept(listenSock, NULL, NULL);
        if (client == INVALID_SOCKET) continue;
        char recvBuf[4096] = { 0 };
        int recvLen = recv(client, recvBuf, sizeof(recvBuf) - 1, 0);
        if (recvLen > 0) HandleHttpRequest(client, std::string(recvBuf, recvLen));
        closesocket(client);
    }
    closesocket(listenSock);
}

// -------------------- SimConnect 管理线程 --------------------
void SimConnectManagerThread() {
    while (running) {
        if (!hSimConnect) {
            HRESULT hr = SimConnect_Open(&hSimConnect, "MSFS MAP Web", NULL, 0, 0, 0);
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
            if (FAILED(SimConnect_CallDispatch(hSimConnect, SimDispatch, NULL))) {
                SimConnect_Close(hSimConnect);
                hSimConnect = NULL;
                g_simConnected = false;
                PostMessage(hMainWnd, WM_APP + 2, 0, 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (hSimConnect) SimConnect_Close(hSimConnect);
}

// -------------------- 窗口过程 --------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static bool btnMinHover = false, btnCloseHover = false;
    static bool mouseTrackActive = false;
    switch (msg) {
    case WM_CREATE:
        hMainWnd = hwnd;
        DetectSystemTheme();
        std::thread(HTTPServerThread, httpPort).detach();
        std::thread(SimConnectManagerThread).detach();
        break;
    case WM_APP + 1: case WM_APP + 2:
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect; GetClientRect(hwnd, &rect);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBmp = CreateCompatibleBitmap(hdc, rect.right, rect.bottom);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        // 背景
        COLORREF bgColor = g_darkMode ? RGB(30, 30, 30) : RGB(250, 250, 250);
        HBRUSH hBrushBG = CreateSolidBrush(bgColor);
        FillRect(memDC, &rect, hBrushBG);
        DeleteObject(hBrushBG);
        // 标题栏渐变
        int titleH = 38;
        TRIVERTEX vertex[2] = {
            {0, 0, GetRValue(g_darkMode ? RGB(60,60,70) : RGB(70,130,200)) << 8, GetGValue(g_darkMode ? RGB(60,60,70) : RGB(70,130,200)) << 8, GetBValue(g_darkMode ? RGB(60,60,70) : RGB(70,130,200)) << 8, 0},
            {rect.right, titleH, GetRValue(g_darkMode ? RGB(80,80,90) : RGB(100,150,220)) << 8, GetGValue(g_darkMode ? RGB(80,80,90) : RGB(100,150,220)) << 8, GetBValue(g_darkMode ? RGB(80,80,90) : RGB(100,150,220)) << 8, 0}
        };
        GRADIENT_RECT gRect = { 0,1 };
        GradientFill(memDC, vertex, 2, &gRect, 1, GRADIENT_FILL_RECT_H);
        // 标题文字
        SetBkMode(memDC, TRANSPARENT);
        SetTextColor(memDC, RGB(255, 255, 255));
        HFONT hTitleFont = CreateFont(16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(memDC, hTitleFont);
        RECT textRect = { 12, 0, rect.right - 100, titleH };
        DrawTextW(memDC, GetAppTitle().c_str(), -1, &textRect, DT_VCENTER | DT_SINGLELINE | DT_LEFT);
        DeleteObject(hTitleFont);
        // 按钮：默认无背景，悬停时显示
        RECT btnMin = { rect.right - 70, 6, rect.right - 35, titleH - 6 };
        RECT btnClose = { rect.right - 35, 6, rect.right, titleH - 6 };
        if (btnMinHover) {
            HBRUSH hMin = CreateSolidBrush(RGB(100, 100, 120));
            FillRect(memDC, &btnMin, hMin);
            DeleteObject(hMin);
        }
        if (btnCloseHover) {
            HBRUSH hClose = CreateSolidBrush(RGB(220, 60, 50));
            FillRect(memDC, &btnClose, hClose);
            DeleteObject(hClose);
        }
        SetTextColor(memDC, RGB(255, 255, 255));
        DrawTextW(memDC, L"－", -1, &btnMin, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(memDC, L"✕", -1, &btnClose, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        // ---------- 主内容 ----------
        std::string ip = GetBestLocalIP();
        std::string url = "http://" + ip + ":" + std::to_string(httpPort);

        // 垂直居中（整体偏上一点）
        int contentStartY = titleH + (rect.bottom - titleH) / 2 - 60;

        // 主提示文字字体（大号）
        HFONT hBigFont = CreateFont(32, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        // 网址字体（中号）
        HFONT hUrlFont = CreateFont(26, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

        SetBkMode(memDC, TRANSPARENT);

        // 第一行：主提示
        RECT rc1 = { 0, contentStartY, rect.right, contentStartY + 48 };
        SelectObject(memDC, hBigFont);
        SetTextColor(memDC, g_darkMode ? RGB(220, 220, 220) : RGB(40, 40, 40));
        std::wstring line1 = GetMainPrompt();
        DrawTextW(memDC, line1.c_str(), -1, &rc1, DT_CENTER | DT_SINGLELINE);

        // 第二行：网址（蓝色）
        RECT rc2 = { 0, contentStartY + 58, rect.right, contentStartY + 106 };
        SelectObject(memDC, hUrlFont);
        SetTextColor(memDC, RGB(0, 120, 215));
        DrawTextA(memDC, url.c_str(), -1, &rc2, DT_CENTER | DT_SINGLELINE);

        DeleteObject(hBigFont);
        DeleteObject(hUrlFont);

        // 底部 SimConnect 状态（字体放大）
        HFONT hSmall = CreateFont(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        SelectObject(memDC, hSmall);
        SetTextColor(memDC, g_simConnected ? RGB(0, 160, 0) : RGB(200, 60, 60));
        RECT statusRect = { 10, rect.bottom - 32, rect.right - 10, rect.bottom - 8 };
        std::wstring simStatus = GetSimStatusText(g_simConnected);
        DrawTextW(memDC, simStatus.c_str(), -1, &statusRect, DT_RIGHT | DT_SINGLELINE);
        DeleteObject(hSmall);

        // 圆角边框
        HPEN hPen = CreatePen(PS_SOLID, 1, g_darkMode ? RGB(80, 80, 100) : RGB(180, 190, 210));
        SelectObject(memDC, hPen);
        SelectObject(memDC, GetStockObject(NULL_BRUSH));
        RoundRect(memDC, 0, 0, rect.right, rect.bottom, 15, 15);
        DeleteObject(hPen);

        BitBlt(hdc, 0, 0, rect.right, rect.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);
        EndPaint(hwnd, &ps);
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleH = 38;
        RECT btnMin = { rect.right - 70, 6, rect.right - 35, titleH - 6 };
        RECT btnClose = { rect.right - 35, 6, rect.right, titleH - 6 };
        if (PtInRect(&btnClose, pt)) PostMessage(hwnd, WM_CLOSE, 0, 0);
        else if (PtInRect(&btnMin, pt)) ShowWindow(hwnd, SW_MINIMIZE);
        else if (pt.y <= titleH) { ReleaseCapture(); SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
        break;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(lParam), HIWORD(lParam) };
        RECT rect; GetClientRect(hwnd, &rect);
        int titleH = 38;
        RECT rMin = { rect.right - 70, 6, rect.right - 35, titleH - 6 };
        RECT rClose = { rect.right - 35, 6, rect.right, titleH - 6 };
        bool prevMin = btnMinHover, prevClose = btnCloseHover;
        btnMinHover = PtInRect(&rMin, pt);
        btnCloseHover = PtInRect(&rClose, pt);
        if (!mouseTrackActive) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            mouseTrackActive = true;
        }
        if (btnMinHover != prevMin || btnCloseHover != prevClose) {
            RECT rc = { rect.right - 80, 0, rect.right, titleH };
            InvalidateRect(hwnd, &rc, FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE:
        btnMinHover = btnCloseHover = false;
        mouseTrackActive = false;
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    case WM_DESTROY:
        running = false;
        httpRunning = false;
        PostQuitMessage(0);
        break;
    default:
        return DefWindowProc(hwnd, msg, wParam, lParam);
    }
    return 0;
}

// -------------------- 主入口 --------------------
int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    EnableHighDPI();
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    DetectSystemLanguage();
    hIconGlobal = LoadIcon(NULL, MAKEINTRESOURCE(IDI_ICON1));
    WNDCLASSW wc = {};
    wc.lpszClassName = L"MSFSMapWeb";
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon = hIconGlobal;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"MSFSMapWeb", GetAppTitle().c_str(),
        WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
        200, 200, 550, 220, NULL, NULL, hInstance, NULL);
    RECT rc; GetClientRect(hwnd, &rc);
    HRGN rgn = CreateRoundRectRgn(0, 0, rc.right + 1, rc.bottom + 1, 20, 20);
    SetWindowRgn(hwnd, rgn, TRUE);
    if (hIconGlobal) {
        SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconGlobal);
        SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconGlobal);
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    WSACleanup();
    if (hIconGlobal) DestroyIcon(hIconGlobal);
    return (int)msg.wParam;
}