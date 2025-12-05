/********************************************************************
 * MSFS MAP 安卓端
 * 版权所有 © 2025-present SunMutian
 * Email: sunmutian88@gmail.com
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.muttix.android.app.msfsmap;

import android.content.Intent;
import android.os.Bundle;
import android.os.Handler;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;

import com.yxing.ScanCodeConfig;
import com.yxing.def.ScanMode;
import com.yxing.def.ScanStyle;

import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.net.Socket;

public class ConnectActivity extends AppCompatActivity {

    private Button scanBtn, connectBtn;
    private EditText ipInput, portInput, pairCodeInput;
    private TextView statusText;
    private ProgressBar loadingAnim;

    private String ip;
    private int port;
    private String pairCode;

    private Socket socket;
    private PrintWriter out;

    private boolean firstPacketReceived = false;
    private final int TIMEOUT_MS = 6000;
    private Handler timeoutHandler = new Handler();
    private Runnable timeoutRunnable;

    public static ConnectActivity instance;

    private static final int ALBUM_QUEST_CODE = 0;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_connect);
        instance = this;

        Permission.checkPermission(this);

        scanBtn = findViewById(R.id.scanBtn);
        connectBtn = findViewById(R.id.connectBtn);
        ipInput = findViewById(R.id.ipInput);
        portInput = findViewById(R.id.portInput);
        pairCodeInput = findViewById(R.id.pairCodeInput);
        statusText = findViewById(R.id.statusText);
        loadingAnim = findViewById(R.id.loadingAnim);

        portInput.setText("5000");

        connectBtn.setOnClickListener(v -> connectToPC());
        scanBtn.setOnClickListener(v -> startScan());
    }

    private void connectToPC() {
        ip = ipInput.getText().toString().trim();
        pairCode = pairCodeInput.getText().toString().trim();
        String portStr = portInput.getText().toString().trim();

        if(portStr.isEmpty()) portStr = "5000";

        try { port = Integer.parseInt(portStr); }
        catch (Exception e) {
            Toast.makeText(this, R.string.te1, Toast.LENGTH_SHORT).show();
            return;
        }

        if(ip.isEmpty() || pairCode.isEmpty()) {
            Toast.makeText(this, R.string.te2, Toast.LENGTH_SHORT).show();
            return;
        }

        showConnectingUI();
        firstPacketReceived = false;

        // 超时处理
        timeoutRunnable = () -> {
            if (!firstPacketReceived) {
                showErrorUI(getString(R.string.conntimeout));
                closeSocket();
            }
        };
        timeoutHandler.postDelayed(timeoutRunnable, TIMEOUT_MS);

        new Thread(() -> {
            try {
                socket = new Socket(ip, port);
                out = new PrintWriter(socket.getOutputStream(), true);
                out.println(pairCode);
                receiveCoordinates();

            } catch (Exception e) {
                runOnUiThread(() -> showErrorUI(getString(R.string.connfail)));
                MapActivity.showDisconnectAlertStatic();
            }
        }).start();
    }

    private void receiveCoordinates() {
        try {
            BufferedReader reader = new BufferedReader(new InputStreamReader(socket.getInputStream()));
            String line;

            while ((line = reader.readLine()) != null) {
                String[] parts = line.split(",");
                if (parts.length != 8 && !line.equals("HEARTBEAT")) {
                    if(line.equals("ERR_PAIR_CODE")){
                        runOnUiThread(() -> showErrorUI(getString(R.string.pairfail)));
                    }else {
                        runOnUiThread(() -> showErrorUI(getString(R.string.connfail)));
                    }
                    closeSocket();
                    return;
                }
                if(!line.equals("HEARTBEAT")){
                    double lat, lon, alt;
                    float heading, pitch, roll, gs, ias;
                    try {
                        lat = Double.parseDouble(parts[0]);
                        lon = Double.parseDouble(parts[1]);
                        alt = Double.parseDouble(parts[2]);
                        heading = Float.parseFloat(parts[3]);
                        pitch = Float.parseFloat(parts[4]);
                        roll = Float.parseFloat(parts[5]);
                        gs = Float.parseFloat(parts[6]);
                        ias = Float.parseFloat(parts[7]);
                    } catch (Exception e) {
                        runOnUiThread(() -> showErrorUI(getString(R.string.connfail)));
                        closeSocket();
                        return;
                    }
                    // 推送到 Map 页面
                    MapActivity.updatePlanePositionStatic(lat, lon, heading, pitch, roll, (float) alt, gs, ias);
                }
                if (!firstPacketReceived) {
                    firstPacketReceived = true;
                    timeoutHandler.removeCallbacks(timeoutRunnable);

                    runOnUiThread(() -> showSuccessUI());

                    Intent intent = new Intent(ConnectActivity.this, MapActivity.class);
                    startActivity(intent);
                }
            }

        } catch (Exception e) {
            runOnUiThread(() -> {
                showErrorUI(getString(R.string.connfail));
                // 通知 Map 页面显示弹窗
                MapActivity.showDisconnectAlertStatic();
            });
        }
    }

    // 扫描QR-Code回调
    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode == RESULT_OK && data != null) {
            switch (requestCode) {
                case ScanCodeConfig.QUESTCODE:
                    // 接收扫码结果
                    Bundle extras = data.getExtras();
                    if (extras != null) {
                        String code = extras.getString(ScanCodeConfig.CODE_KEY);
                        parseQRCode(code);
                    }
                    break;
                case ALBUM_QUEST_CODE:
                    // 接收图片识别结果
                    String code = ScanCodeConfig.scanningImage(this, data.getData());
                    parseQRCode(code);
                    break;
                default:
                    break;
            }
        }
    }

    // 显示正在连接中UI
    private void showConnectingUI() {
        statusText.setText(R.string.conning);
        statusText.setTextColor(0xFF007BFF);
        loadingAnim.setVisibility(View.VISIBLE);
    }

    // 显示连接成功UI
    private void showSuccessUI() {
        statusText.setText(R.string.connok);
        statusText.setTextColor(0xFF00AA00);
        loadingAnim.setVisibility(View.GONE);
    }

    // 显示连接失败UI
    private void showErrorUI(String msg){
        statusText.setText(msg);
        statusText.setTextColor(0xFFFF0000);
        loadingAnim.setVisibility(View.GONE);
    }

    // 关闭Socket
    private void closeSocket(){
        try { if(socket != null) socket.close(); } catch (Exception ignored){}
    }

    // 扫描QR-Code
    private void startScan() {
        Permission.checkPermission(this);
        ScanCodeConfig.create(this)
                .setStyle(ScanStyle.WECHAT)
                .setPlayAudio(true)
                .setLimitRect(true)
                .setScanSize(1000, 0, 0)
                //是否显示边框上四个角标 true ： 显示  false ： 不显示
                .setShowFrame(true)
                //设置边框上四个角标圆角  单位 /dp
                .setFrameRadius(12)
                //设置边框上四个角宽度 单位 /dp
                .setFrameWith(4)
                //设置边框上四个角长度 单位 /dp
                .setFrameLength(25)
                //设置是否显示边框外部阴影 true ： 显示  false ： 不显示
                .setShowShadow(true)
                //设置扫码条运动方式   ScanMode.REVERSE : 往复运动   ScanMode.RESTART ：重复运动    默认ScanMode.RESTART
                .setScanMode(ScanMode.REVERSE)
                .setIdentifyMultiple(true)
                .setQrCodeHintDrawableWidth(120)
                .setQrCodeHintDrawableHeight(120)
                .setStartCodeHintAnimation(true)
                .setQrCodeHintAlpha(0.5f)
                .buidler()
                .start(ScanActivity.class);
    }

    // 解析来自QR-Code的数据，并且开始连接MSFS Map电脑端。
    private void parseQRCode(String content) {
        try {
            String[] parts = content.split(";");
            String[] ipPort = parts[0].split(":");
            ip = ipPort[0];
            port = Integer.parseInt(ipPort[1]);
            pairCode = parts[1].split(":")[1];

            ipInput.setText(ip);
            portInput.setText(String.valueOf(port));
            pairCodeInput.setText(pairCode);

            connectToPC();
        } catch (Exception e) {
            Toast.makeText(this, R.string.qrerr, Toast.LENGTH_SHORT).show();
        }
    }
}
