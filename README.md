# Custom Firmware for Dewenwils EcoPlug

This project provides custom ESP32-based firmware for:
- [EcoPlug Outdoor Wireless Controller](http://www.eco-plugs.net/en/products/show.php?id=70)
- [Dewenwils Heavy Duty Outdoor Smart Plug](https://www.amazon.com/dp/B07PP2KNNH)

## ⚠️ Background

Originally shipped with an ESP8266-based module. Reflash attempt using [ESPHome’s HOWT01A instructions](https://devices.esphome.io/devices/Dewenwils-Heavy-Duty-40A-Outdoor-Plug-HOWT01A) did not work on my unit.

ESP8266 was replaced with an ESP32.

## 💡 Purpose

This firmware is used to control a **pool filter pump** with:
- WiFi provisioning via fallback AP
- Web-based captive portal for SSID/password setup
- Automatic STA retry
- Optional OTA update support

## 🛠️ Features

- Independent filter and refill timers with weekly schedules
- Persistent timer settings and timezone configuration
- Wi-Fi provisioning portal with network scanning
- Browser-based firmware updates
- Captive portal fallback when saved Wi-Fi credentials cannot connect

## 🚀 Build and Upload

The scripts use ESP-IDF 6.1 packages installed through PlatformIO by default, including its Python environment and compiler tools. They invoke `idf.py` directly. Override `IDF_PATH` or `IDF_PYTHON_ENV_PATH` when using a different ESP-IDF installation.

Build for the project's default ESP32 target:

```bash
./scripts/build.sh
```

Build for the ESP32-C6 target instead:

```bash
./scripts/build.sh esp32c6
```

Build, flash over USB, and open the serial monitor:

```bash
./scripts/build-upload-monitor.sh
```

The separate `./scripts/upload.sh` and `./scripts/monitor.sh` commands are also available. Set `IDF_PORT` when more than one serial device is connected, for example `IDF_PORT=/dev/cu.usbserial-0001 ./scripts/upload.sh`. The application binary for browser-based updates is generated under `build/esp32/PoolTimers.bin` (or the selected target directory).

## 📶 First-Time WiFi Provisioning

When no STA credentials are saved, the device starts in AP mode.

1. Connect to AP SSID: `SetupAP`
2. AP password: `setup123`
3. Open `http://192.168.4.1/wifi` (or use the captive portal redirect).
4. Select your Wi-Fi network, enter its password, then submit to save and reboot.

## 🧯 WiFi Recovery

If saved credentials no longer connect, the manager starts its setup access point after retrying:

1. Connect to `SetupAP` using password `setup123`.
2. Open `http://192.168.4.1/wifi`.
3. Select the current Wi-Fi network and save the credentials.

## 🔧 Serial Log Hint

Wi-Fi lifecycle messages use the `WifiManager` ESP-IDF log tag. With no saved station credentials, the setup access point is started for provisioning.
