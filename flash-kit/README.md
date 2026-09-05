# flash-kit — ESP-01S 烧录包

本目录包含编译好的固件，可在**任意电脑**上烧录，无需安装 Arduino 环境。

## 文件

- `autowaterfeed.ino.bin` — 固件（ESP-01S，1MB Flash + 64KB 文件系统布局，`esp8266:esp8266:generic:eesz=1M64`）

## 烧录步骤

```bash
# 1. 安装 esptool（需要 Python 3）
pip install esptool

# 2. 接线（CH340 USB-TTL 适配器 ↔ ESP-01S）
#    适配器3.3V → VCC（电流不足时用独立 3.3V，共地）
#    适配器GND  → GND
#    适配器TX   → RX
#    适配器RX   → TX
#    GPIO0      → GND（下载模式，烧完断开）
#    CH_PD(EN)  → 3.3V
#    GPIO2      → 悬空

# 3. 查端口：Windows 设备管理器 / macOS: ls /dev/tty.usbserial-* / Linux: ls /dev/ttyUSB*

# 4. 烧录（裸 CH340 无自动复位电路，必须 --before no-reset）
esptool.py --port COM4 --before no-reset write_flash 0x0 autowaterfeed.ino.bin
```

烧完断开 GPIO0 的线并复位，串口 115200 应输出 `=== autowaterfeed boot ===` 和 IP 地址。

## 常见故障

| 现象 | 原因 | 解决 |
|---|---|---|
| 端口接上 ESP 就打不开 / "device not functioning" | 适配器 3.3V 供电不足，ESP 电流尖峰拖垮 CH340 | ESP 改独立 3.3V 供电，共地 |
| `A fatal error occurred: Failed to connect to ESP8266` | 未进下载模式 / TX-RX 没交叉 | GPIO0 接地后重新上电；核对 TX↔RX |
| 烧录成功但无输出 / 反复重启 | 固件 Flash 布局与芯片不符 | 本包固件按 1MB 布局编译，仅适用于 ESP-01S |
