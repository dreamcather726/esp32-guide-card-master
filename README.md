# 九川 ESP32-S3 4G 固件

适用于搭载 ML307 蜂窝模块、16 MB Flash 的九川 ESP32-S3。上电后自动建立 USB RNDIS 网络。已有链路可直接获取 IP，需要拨号时通过 USB AT 发送 `AT+MDIALUP=1,1`。DHCP 获取 IPv4 地址后配置 DNS，断线或拨号后迟迟未获 IP 时会尝试恢复。

## 编译与烧录

使用 ESP-IDF 5.5.4。在 ESP-IDF 终端执行以下命令，烧录前将 `COMx` 替换为实际串口。

```sh
idf.py build
idf.py -p COMx flash monitor
```

应用镜像为 `build/jiuchuan_4g.bin`。使用发布目录里的 `*-full.bin` 时，从地址 `0x0` 烧录。该版本采用单应用分区，从旧版导览固件迁移时应完整烧录，原有 Flash 设置会被覆盖。

用 115200 波特率查看串口。出现 `4G IP OK` 时，设备已从 ML307 获得 IPv4 地址，日志同时显示网关。公网连通性和断线恢复应在接有有效 SIM 的实机上验证。

## 开发与测试

[网络入口](main/main.cc)启动[现有 ML307 USB RNDIS 实现](main/boards/common/rndis_board.cc)。[独立服务端](server/README.md)与固件构建分开。

```sh
python tests/test_rndis_source.py
python tests/run_rndis_host_tests.py
python tests/run_usb_transport_host_tests.py
```

主机测试需要支持 C++17 的编译器和支持 AddressSanitizer、UndefinedBehaviorSanitizer 的 C 编译器。
