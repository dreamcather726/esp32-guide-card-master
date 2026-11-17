# 九川板 XiaoZhi 2.1.0 迁移记录

本目录以 XiaoZhi 2.1.0 为核心基线，九川硬件只通过 2.1.0 的 `Board`、`Application`、`AudioService` 和 `Protocol` 扩展点接入，不再复制 2.0.5 的主循环或双网络板实现。

## 板级配置

- LCD：ST7789，240x135，SPI3，SCK 47、MOSI 38、DC 39、CS 48、RESET 40。
- 背光：GPIO45，低有效。
- ES8311：I2C SDA 1、SCL 2；I2S MCLK 13、WS 7、BCLK 8、DIN 9、DOUT 6；PA 14。
- 按键：角色 GPIO0、音量加 GPIO11、音量减 GPIO15、PTT GPIO16。
- SDMMC 1-bit：CLK 42、CMD 41、D0 21。
- RGB：GPIO46，12 颗。
- ML307/外设电源使能：GPIO5。

## 业务逻辑

- GPIO0 调用 `Application::SwitchRoleVoiceBinding()`，通过协议发送 `role_switch/next`。
- 角色切换介绍和待命状态的主动 TTS 播放结束后回到 Idle；角色切换等待标志 15 秒后自动失效。
- GPIO16 长按进入 manual-stop 聆听，松开发送 stop-listening 并立即离开聆听状态。
- `/sdcard/SafetyReminder.mp3` 在音频服务启动后入队，不依赖蜂窝网络或 OTA 激活。
- `play_sd_audio` 采用有界串行队列，支持 WAV/OGG、安全相对路径和 16 位 PCM WAV 重采样。

## USB 网络

`RndisBoard` 使用 2.1.0 的异步网络事件模型，不阻塞 `Application::Initialize()`。固件会同时监听 ML307 USB 设备事件；遇到不含 RNDIS 的初始 USB 组合时，按 interface 2、8、4 和其余 vendor bulk interface 的顺序探测 AT 口，发送 `AT+MDIALUP=1,1`，之后由 RNDIS 驱动处理重新枚举和 DHCP。

项目内置了 `components/iot_usbh_rndis` 覆盖组件，用于修复 0.3.1 的 RNDIS QUERY_MSG 请求长度。不要删除该覆盖组件，否则 ML307 可能在 OID 查询阶段断开。
