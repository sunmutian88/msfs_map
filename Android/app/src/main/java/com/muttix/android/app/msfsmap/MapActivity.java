/********************************************************************
 * MSFS MAP 安卓端
 * 版权所有 © 2025-present SunMutian
 * Email: sunmutian88@gmail.com
 * 时间: 2025-12-05
 * 本软件遵循 CC BY-NC-SA 4.0 协议，不得用于商业用途！
 ********************************************************************/

package com.muttix.android.app.msfsmap;

import android.content.res.Configuration;
import android.os.Bundle;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import androidx.appcompat.app.AlertDialog;
import androidx.appcompat.app.AppCompatActivity;

import com.amap.api.maps.AMap;
import com.amap.api.maps.CameraUpdateFactory;
import com.amap.api.maps.MapView;
import com.amap.api.maps.MapsInitializer;
import com.amap.api.maps.model.BitmapDescriptorFactory;
import com.amap.api.maps.model.LatLng;
import com.amap.api.maps.model.Marker;
import com.amap.api.maps.model.MarkerOptions;
import com.amap.api.maps.model.Polyline;
import com.amap.api.maps.model.PolylineOptions;

public class MapActivity extends AppCompatActivity {

    private MapView mapView;
    private AMap aMap;
    private static MapActivity instance;

    private LatLng planePos;
    private Marker planeMarker;
    private LatLng navTarget;
    private Marker navMarker;
    private Polyline navLine;

    private TextView coordText;
    private LinearLayout buttonPanel;
    private Button navBtn;

    private boolean navigating = false;
    private boolean firstUpdate = true;

    public static MapActivity activity;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        instance = this;
        activity = this;

        setContentView(R.layout.activity_map);

        mapView = findViewById(R.id.mapView);
        mapView.onCreate(savedInstanceState);

        coordText = findViewById(R.id.coordText);
        buttonPanel = findViewById(R.id.buttonPanel);
        navBtn = findViewById(R.id.navBtn);

        aMap = mapView.getMap();
        MapsInitializer.updatePrivacyAgree(this,true);
        MapsInitializer.updatePrivacyShow(this,true,true);

        if (isSystemDarkMode())
            aMap.setMapType(AMap.MAP_TYPE_NIGHT);
        else
            aMap.setMapType(AMap.MAP_TYPE_NORMAL);

        navBtn.setOnClickListener(v -> {
            if (!navigating) {
                Toast.makeText(this, R.string.tip1, Toast.LENGTH_SHORT).show();
                navigating = true;
                navBtn.setText(R.string.tx);
            } else {
                endNavigation();
            }
        });

        aMap.setOnMapLongClickListener(latLng -> {
            if (navigating)
                setNavigationTarget(latLng);
        });

        findViewById(R.id.rtp).setOnClickListener(v -> {
            if (planePos != null)
                aMap.moveCamera(CameraUpdateFactory.changeLatLng(planePos));
        });
    }

    public static void updatePlanePositionStatic(double lat, double lon, float heading, float pitch,
                                                 float roll, float altitude, float gpsGroundSpeed,
                                                 float indicatedAirspeed) {
        if (instance != null) {
            instance.runOnUiThread(() ->
                    instance.updatePlanePosition(lat, lon, heading, pitch, roll, altitude, gpsGroundSpeed, indicatedAirspeed)
            );
        }
    }

    private void updatePlanePosition(double lat, double lon, float heading, float pitch, float roll,
                                     float altitude, float gpsGroundSpeed, float indicatedAirspeed) {

        planePos = new LatLng(lat, lon);

        if (planeMarker == null) {
            planeMarker = aMap.addMarker(new MarkerOptions()
                    .position(planePos)
                    .anchor(0.5f, 0.5f)
                    .icon(BitmapDescriptorFactory.fromResource(R.mipmap.navigation_)));
            planeMarker.setFlat(true);
        } else {
            planeMarker.setPosition(planePos);
            planeMarker.setRotateAngle(heading);
        }

        String latDir = lat >= 0 ? "N" : "S";
        String lonDir = lon >= 0 ? "E" : "W";

        StringBuilder sb = new StringBuilder();
        sb.append(String.format("飞机数据\n纬度: %.6f %s\n经度: %.6f %s\n高度: %.1f ft\n航向: %.1f°\n",
                Math.abs(lat), latDir, Math.abs(lon), lonDir, altitude, heading));
        sb.append(String.format("俯仰: %.1f°\n横滚: %.1f°\n地速: %.2f m/s\n空速: %.2f m/s",
                pitch, roll, gpsGroundSpeed, indicatedAirspeed));

        if (navigating && navTarget != null) {
            double distance = getDistance(planePos, navTarget);
            sb.append("\n剩余导航距离: ").append(String.format("%.1f km", distance));
        }

        coordText.setText(sb.toString());

        if (firstUpdate) {
            aMap.moveCamera(CameraUpdateFactory.changeLatLng(planePos));
            firstUpdate = false;
        } else {
            aMap.animateCamera(CameraUpdateFactory.newLatLng(planePos), 500, null);
        }

        if (navigating && navTarget != null) {
            if (navLine == null) {
                navLine = aMap.addPolyline(new PolylineOptions()
                        .add(planePos, navTarget).width(5).color(0xFF00AAFF));
            } else {
                navLine.setPoints(java.util.Arrays.asList(planePos, navTarget));
            }
        }
    }

    private void setNavigationTarget(LatLng target) {
        navTarget = target;

        if (navMarker != null) navMarker.remove();
        navMarker = aMap.addMarker(new MarkerOptions()
                .position(navTarget)
                .anchor(0.5f, 0.5f)
                .icon(BitmapDescriptorFactory.fromResource(R.mipmap.n)));

        if (navLine != null) navLine.remove();
        navLine = aMap.addPolyline(new PolylineOptions()
                .add(planePos, navTarget)
                .width(5)
                .color(0xFF00AAFF));
    }

    public static void showDisconnectAlertStatic() {
        if (activity == null) return;

        activity.runOnUiThread(() -> {
            new AlertDialog.Builder(activity)
                    .setTitle("连接断开")
                    .setMessage("飞机数据连接已断开，请重新连接。")
                    .setCancelable(false)
                    .setPositiveButton("确定", (d, w) -> {
                        activity.finish(); // 返回连接页面
                    })
                    .show();
        });
    }

    private double getDistance(LatLng a, LatLng b) {
        double lat1 = Math.toRadians(a.latitude);
        double lon1 = Math.toRadians(a.longitude);
        double lat2 = Math.toRadians(b.latitude);
        double lon2 = Math.toRadians(b.longitude);
        double R = 6371;
        double dLat = lat2 - lat1;
        double dLon = lon2 - lon1;
        double hav = Math.sin(dLat/2)*Math.sin(dLat/2)
                + Math.cos(lat1)*Math.cos(lat2)*Math.sin(dLon/2)*Math.sin(dLon/2);
        double c = 2 * Math.atan2(Math.sqrt(hav), Math.sqrt(1-hav));
        return R * c;
    }

    private void endNavigation() {
        navigating = false;
        navBtn.setText(R.string.tx2);

        if (navMarker != null) navMarker.remove();
        if (navLine != null) navLine.remove();

        navTarget = null;
    }

    private boolean isSystemDarkMode() {
        int nightModeFlags = getResources().getConfiguration().uiMode &
                Configuration.UI_MODE_NIGHT_MASK;
        return nightModeFlags == Configuration.UI_MODE_NIGHT_YES;
    }

//    @Override
//    public void onBackPressed() {
//        // super.onBackPressed();
//        activity.runOnUiThread(() -> {
//            new AlertDialog.Builder(activity)
//                    .setTitle("您确定要退出吗？")
//                    .setMessage("退出将返回至连接页面。")
//                    .setCancelable(false)
//                    .setPositiveButton("是", (d, w) -> {
//                        activity.finish(); // 返回连接页面
//                    })
//                    .setPositiveButton("否", (d, w) -> {
//                        // 啥也不做
//                    })
//                    .show();
//        });
//    }
    @Override protected void onResume() { super.onResume(); mapView.onResume(); }
    @Override protected void onPause() { super.onPause(); mapView.onPause(); }
    @Override protected void onDestroy() { super.onDestroy(); mapView.onDestroy(); instance = null; activity = null; }
}
