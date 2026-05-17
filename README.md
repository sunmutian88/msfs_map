# MSFS Map – 纯 Web 版

[![License](https://img.shields.io/badge/License-CC%20BY--NC--SA%204.0-lightgrey)](https://creativecommons.org/licenses/by-nc-sa/4.0/)

**MSFS Map** 是一个通过 SimConnect API 连接 Microsoft Flight Simulator 的实时地图工具。  
**本版本为纯 Web 版**：电脑端运行后，自动启动内置 Web 服务器，同一局域网内的任何设备（手机、平板、电脑）只需用浏览器打开显示的地址，即可查看飞机实时位置、飞行数据和逆地理编码地址。

> **不再需要手机客户端，也无需配对码**。一切通过浏览器完成。

## ✨ 主要特点

- 🖥️ **纯 Web 访问** – 电脑端只显示局域网地址，手机/平板扫码或输入地址即可使用。
- 🗺️ **交互式地图** – 基于 Leaflet + OpenStreetMap，支持缩放、拖动，可一键跟随飞机。
- 📍 **自动地址解析** – 实时显示飞机所在国家、省/州、城市（通过 Nominatim API）。
- 📊 **完整飞行数据** – 经纬度、高度、航向、地速、俯仰/横滚等一目了然。
- 🌓 **深色/浅色主题** – Windows 窗口自适应系统主题，网页地图明亮清晰。
- 🌐 **多语言界面** – 桌面端根据系统语言自动切换中英文；网页端也内置中英文（JS 动态切换）。

## 📸 截图

*（建议添加：程序窗口截图 + 手机浏览器地图截图）*

## 🚀 快速开始

### 下载预编译 exe（推荐）
1. 前往 [Releases](https://github.com/sunmutian88/msfs_map/tree/web_version/releases) 下载最新 `MSFSMap_Web.exe`。
2. 确保电脑与手机/平板连接 **同一局域网**。
3. 运行 `MSFSMap_Web.exe`，窗口会显示类似 `http://192.168.1.100:8080` 的地址。
4. 用手机/平板浏览器扫描二维码，或直接输入该地址。
5. 浏览器中即可看到飞机位置（需要 MSFS 正在运行且飞机在空中或地面）。