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

## BLE 信标日志

`2.1.25` 在开机时启动被动扫描，不连接周围设备。打开 115200 波特率的串口监视器，筛选 `BleBeacon`：

```text
I (...) BleBeacon: Passive BLE scan started
I (...) BleBeacon: MAC=12:34:56:78:9A:BC type=public RSSI=-67 dBm
```

扫描接收到的 BLE 广播都会按设备地址记录，同一地址的重复报告由控制器过滤，约每 5 秒重新采样。日志使用广播地址：如果信标启用随机地址，显示的 MAC 可能变化。此版本扫描传统 BLE 广播，不解析信标厂商数据或上传扫描结果。

本固件使用 `sdkconfig.jiuchuan-s3`。从旧版构建目录继续编译时，请同步启用该配置里的 NimBLE Observer 选项，或重新生成 sdkconfig。
