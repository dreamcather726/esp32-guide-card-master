# jiuchuan-xiaozhi-sound
九川科技小智AI音箱

## 🛠️ 编译指南
**开发环境**：ESP-IDF v5.5.4

### 编译步骤：
> ⚠️ **提示**：若在编译过程中访问在线库失败，可以尝试切换加速器状态，或修改 [idf_component.yml] 文件，替换为国内镜像源。

1. 使用 VSCode 打开项目文件夹；
2. 清除工程（Clean Project）；
3. 设置 ESP-IDF 版本为 `v5.5.4`；
4. 点击 VSCode 右下角提示，生成 [compile_commands.json] 文件；
5. 设置目标设备为 `[esp32s3] -> [JTAG]`；
6. 打开 **SDK Configuration Editor**；
7. 设置 **Board Type** 为 **九川科技**；
8. 保存配置并开始编译。

## 🔌 烧录步骤
1. 使用数据线连接电脑与音箱；
2. 关闭设备电源后，长按电源键不松手；
3. 在烧录工具中选择对应的串口（COM Port）；
4. 点击烧录按钮，选择 UART 模式；
5. 烧录完成前请勿松开电源键。

上电黑屏时，先在 115200 波特率串口确认 `App version`。从 `2.1.29` 起，九川板在 PWM 接管时直接应用保存的亮度；成功初始化后会打印 `[LCD_BOOT] ST7789 display initialized` 和 `[LCD_BOOT] Startup logo prepared`。完整烧录包用地址 `0x0`；`-ota.bin` 只用于升级应用，不能作为完整包烧到 `0x0`。

## BLE 信标日志

设备使用被动扫描，不连接周围设备。4G 模式在 ML307 的 AT 接口就绪后开始扫描；Wi-Fi 模式开机开始扫描。打开 115200 波特率的串口监视器，筛选 `BleBeacon`：

```text
I (...) BleBeacon: Passive BLE scan started; MAC contains D9:62:62
I (...) BleBeacon: MAC=D9:62:62:00:00:0D RSSI=-83 dBm
```

从 `2.1.28` 起，只打印广播地址中包含 `D9:62:62` 的记录。同一地址的重复报告由控制器过滤，约每 5 秒重新采样。信标使用随机地址时，地址变化可能影响匹配。扫描使用传统 BLE 广播的地址和信号强度。

从 `2.1.34` 起，业务 MQTT 会在 `device/broadcast/online` 的 `KJ` 包中带上 `bleBeacons`。MQTT 连接成功时发送状态包，在线后通常每 60 秒更新一次。每个 MAC 保留最近一次有效 RSSI，最多记录 16 台，90 秒内没有有效采样的记录不会进入上报。数组为空表示没有符合条件的近期记录。

```json
{"cmd":"KJ","bleBeacons":[{"macAddress":"D9:62:62:00:00:0D","rssi":-83}]}
```

## 屏幕诊断

从 `2.1.35` 起，长按回放键约 1.5 秒打开诊断页，再长按关闭。页面显示 USB/IMEI、网络 IP、二维码、WAV、最近一次有效的 BLE MAC 和 RSSI、业务 MQTT、定位 MQTT，以及最近一次 KJ 发送结果。正常界面显示故障提示，开机二维码仍优先占用屏幕。

`POS MQTT SUB OK` 表示定位主题已订阅，`POS RX FIX=1 SAT=15` 表示收到有效定位和 15 颗卫星。BLE 行末数值的单位为 dBm。`KJ TX #3 BLE=2` 表示本次开机 MQTT 客户端已接受第 3 个 KJ 包，包里有 2 个信标。KJ 使用 QoS 0，服务端是否收到需查看服务端订阅记录。

从 `2.1.37` 起，二维码进度条按 HTTP 已接收字节更新。服务器提供文件长度时显示百分比，长度未知时显示已接收字节数。写完后显示 `QR VERIFYING`，校验并保存成功后进入二维码页面。

`2.1.38` 修复了精简版格式化函数造成的 `QR DOWNLOADING zu B`。`QR HTTPS CONNECT` 表示正在建立连接，`QR WAIT HTTP HEADER` 表示请求已发出、正在等服务器响应头，`QR HTTP READ ERR R1024` 表示收到 1024 字节后读取失败。

写卡失败时，屏幕保留类似 `QR E5 R4096 W0 S109` 的信息。`R` 是网络已接收字节，`W` 是文件写入接口已接收字节，`S` 是捕获到的 SDMMC 错误码（正数按十六进制显示）。`S107` 表示超时，`S109` 表示 CRC 校验错误。完整文件保存成功以 `QR SAVED TF` 为准。

`2.1.39` 会捕获 SDMMC 扇区读写的返回码，覆盖卡命令尚未发出时的错误。`S101` 表示 SDMMC 操作内存分配失败。`S--` 表示本次写入失败时没有捕获到扇区错误码，仍需检查 FatFs 或文件写入链路。

本固件使用 `sdkconfig.jiuchuan-s3`。从旧版构建目录继续编译时，请同步启用该配置里的 NimBLE Observer 选项，或重新生成 sdkconfig。
