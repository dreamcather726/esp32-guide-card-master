# 九川 ESP32-S3 小智固件

本固件用于搭载 ML307、ST7789 屏幕和 ES8311 音频编解码器的九川 ESP32-S3（16 MB Flash）。上电后通过 ML307 USB RNDIS 联网，可按语音键或说“你好小智”开始对话。语音识别和回复依赖小智服务。

开机按本机 IMEI 从 TF 卡读取二维码，缺失时联网补下载。屏幕显示初始化、待命时钟、安全提示、当地讲解、聆听及对话状态。四角显示实测信号、电池百分比、卫星数和 Scene 编号。

## 编译与烧录

使用 ESP-IDF 5.5.4。先从新的构建目录生成配置，再烧录到实际串口。

```sh
idf.py -B build_jiuchuan_voice -D SDKCONFIG=build_jiuchuan_voice/sdkconfig build
idf.py -B build_jiuchuan_voice -D SDKCONFIG=build_jiuchuan_voice/sdkconfig -p COMx flash monitor
```

从 4G 精简版 2.1.40 升级时，将完整 BIN 从地址 `0x0` 烧录，以写入分区表、应用和语音字体资源。完整烧录会覆盖 Flash 中的原有设置。串口使用 115200 波特率。出现 `4G IP OK` 表示已获得 IPv4 地址和网关。

[九川板使用说明](main/boards/jiuchuan-s3/README.md)包含定位数据来源和屏幕诊断。[独立服务端](server/README.md)继续单独维护。
