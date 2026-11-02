# ESP32-S3-LVGL 电子塔罗牌

基于 **ESP32-S3 + ESP-IDF + LVGL 9** 的 1.54 寸 IPS 屏（ST7789，240x240 SPI）显示项目。默认上电进入「电子塔罗牌」界面：随机抽取一张大阿尔克纳（22 张），50% 概率正位/逆位，逆位时牌面 180° 翻转，并配有淡入 + 上滑 + 金色光晕收束的抽牌动画。

> 另内置一套屏幕验证 Demo（彩色条 / 网格几何 / 动画 / 雷达扫描 / 粒子星空 / 彩虹波浪），可通过宏一键切换，用于新屏幕的点亮点检与 SPI 带宽压测。

## 硬件准备

| 硬件 | 说明 |
|---|---|
| 主控 | ESP32-S3 N16R8（16MB Flash + 8MB PSRAM） |
| 屏幕 | 1.54" IPS 240x240，ST7789 驱动，SPI 接口 |
| 按键 | 开发板板载 BOOT 键（GPIO0） |

## 接线

| 屏幕引脚 | ESP32-S3 引脚 | 说明 |
|---|---|---|
| GND  | GND    | 地 |
| VCC  | 3V3    | 电源 |
| SCL  | GPIO12 | SPI2 SCK |
| SDA  | GPIO11 | SPI2 MOSI |
| RES  | GPIO8  | 复位 |
| DC   | GPIO9  | 命令/数据 |
| CS   | GPIO10 | 片选 |
| BLK  | GPIO14 | 背光 |

引脚定义位于 [main/ESP32-S3-LVGL.c](main/ESP32-S3-LVGL.c#L44-L49)，可按需修改。

## 软件环境

- ESP-IDF **>= 5.1**
- LVGL `~9.2.0`（由 [main/idf_component.yml](main/idf_component.yml) 声明，首次编译自动拉取）
- Python 3（仅重新生成卡面像素数据时需要）

## 编译与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash monitor
```

Windows 下串口形如 `COMx`。退出 monitor 用 `Ctrl+]`。

## 使用说明

- **抽牌**：短按 BOOT 键随机抽一张；每 5 秒也会自动抽一张（`AUTO_DRAW_MS` 可在 [main/tarot_ui.c](main/tarot_ui.c#L45) 中调整）
- **正位/逆位**：牌名分别以金色/紫色显示，逆位时牌面倒置
- **切换屏幕验证 Demo**：把 [main/ESP32-S3-LVGL.c](main/ESP32-S3-LVGL.c#L63) 中的 `DEMO_ENABLE` 改为 `1` 后重新编译，将循环播放彩色条、网格几何、弹跳动画与代码特效

## 项目结构

```
├── CMakeLists.txt            # 顶层工程文件
├── partitions.csv            # 自定义分区表（app 4MB）
├── sdkconfig.defaults        # LVGL / Flash / 分区表默认配置
├── main/
│   ├── ESP32-S3-LVGL.c       # 入口：LCD 初始化 + Demo/塔罗界面切换
│   ├── tarot_ui.c/.h         # 塔罗牌界面（自实现 LVGL flush，不依赖 esp_lvgl_port）
│   ├── tarot_data.c/.h       # 22 张牌像素画 + 牌名释义数据（脚本自动生成）
│   ├── tarot_font_16.c       # 16px 中文字库
│   └── wifi_mgr.c/.h         # WiFi 配网模块（SoftAP 网页配网 + SNTP 对时，未编译启用）
└── tools/
    └── gen_tarot.py          # 卡面像素画生成脚本
```

## 自定义

### 修改卡面 / 语料

编辑 [tools/gen_tarot.py](tools/gen_tarot.py) 后重新生成：

```bash
python3 tools/gen_tarot.py
```

会自动更新 `main/tarot_data.c/.h` 并输出预览图 `tools/preview_tarot.png`。卡面为 30x40 原生 RGB565，运行时最近邻放大 4x 到 120x160。

### 屏幕显示异常排查

相关宏位于 [main/ESP32-S3-LVGL.c](main/ESP32-S3-LVGL.c#L52-L60)：

| 现象 | 调整 |
|---|---|
| 花屏 / 不稳定 | `LCD_PCLK_HZ` 从 40MHz 降到 20MHz |
| 屏幕上/下出现雪花或整体偏移 | 调整 `LCD_X_GAP` / `LCD_Y_GAP` |
| 颜色像负片 | `LCD_INVERT` 改为 0 |
| 红蓝互换 | `LCD_BGR` 改为 0 |

### WiFi 配网模块（预留）

[main/wifi_mgr.c](main/wifi_mgr.c) 实现了完整的配网流程：无凭据时开启 `ESP-Clock-xxxx` 热点，手机浏览器访问 `http://192.168.4.1` 填写 WiFi 信息保存并重启，联网后通过 `ntp.aliyun.com` 对时（时区 CST-8）。该模块当前未加入 [main/CMakeLists.txt](main/CMakeLists.txt) 编译，可作为时钟类应用的基础接入。
