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
I (...) BleBeacon: MAC=D9:62:62:00:00:0D type=public RSSI=-83 dBm
```

从 `2.1.28` 起，只打印广播地址中包含 `D9:62:62` 的记录。同一地址的重复报告由控制器过滤，约每 5 秒重新采样。信标使用随机地址时，地址变化可能影响匹配。扫描只处理传统 BLE 广播，不解析信标厂商数据或上传结果。

本固件使用 `sdkconfig.jiuchuan-s3`。从旧版构建目录继续编译时，请同步启用该配置里的 NimBLE Observer 选项，或重新生成 sdkconfig。
