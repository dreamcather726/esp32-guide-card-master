# 九川 ESP32-S3 小智

## 按键和页面

- 按住语音键说话，松开后结束输入。也可说“你好小智”开始对话。
- 开机时优先显示二维码和 IMEI 约 3 秒。初始化显示 Vanex 标志，待命显示时钟和关联状态。
- 本地安全提示或讲解开始播放时，屏幕显示扬声器图标、标题和内容。聆听和小智回复会切换到对应页面。
- 长按回放键约 1.5 秒打开或关闭诊断页，可看语音、USB、4G IP、二维码、WAV、业务与定位 MQTT。

语音键按下后，主画面短暂显示进度，失败信息保留到该阶段有新结果。诊断页一直显示最近的状态。

| 屏幕提示 | 含义 |
| --- | --- |
| `PTT DOWN` / `PTT HELD` | 检测到按键 / 已触发长按。 |
| `LINK MQTT NO ENDPOINT` / `LINK WS NO URL` | 缺少当前语音协议的连接配置。 |
| `LINK TLS SETUP 8017` | TLS 初始化失败。串口会记录 mbedTLS 底层错误和内部 RAM、PSRAM 的最大可用块。 |
| `LINK … NO HELLO` / `LINK WS NO USER CONTEXT` | 未收到服务器握手 / 用户上下文。 |
| `VOICE AUDIO TX OK` / `VOICE OPUS NO TX` | 本轮至少发送一帧音频 / 已编码，松键时发送尚未成功。 |
| `VOICE MIC OR RATE ERR` / `VOICE NO MIC SAMPLES` | 麦克风读取或重采样失败 / 尚未采集到一块音频。 |
| `VOICE AFE NOT READY` / `VOICE AFE NO FRAME` / `VOICE OPUS ERR` | 语音处理未就绪 / 已采集但未形成处理后的帧 / 编码失败。 |
| `VOICE STT OK` / `VOICE TTS START` | 收到识别文字 / 收到语音回复开始事件。 |

## 四角图标

| 位置 | 数据来源 |
| --- | --- |
| 左上 | ML307 的 `AT+CSQ`。1–7 一格，8–15 两格，16–27 三格，28–31 四格。0 或未知时隐藏。 |
| 右上 | 电池 ADC 电压。3.2 V 对应 0%，3.7 V 对应 100%，中间线性换算并保留采样平滑。 |
| 左下 | 定位 MQTT 的 `nsat`，失去有效定位时卫星图标闪烁。 |
| 右下 | 定位 MQTT 的 `Scene` 或 `scene`。 |

位置数据按设备 IMEI 订阅。另一台构建机需要提供本地 `position_mqtt_local.h` 配置，具体字段见[配置示例](position_mqtt_local.h.example)。配置文件由 Git 忽略。

开机资源沿用 TF 卡上的 `<IMEI>.jpg` 和 `start.wav`。卡上缺失时，4G 就绪后按原地址补下载。TF 写卡故障在诊断页显示已接收字节、已写字节和 SDMMC 错误码。

## 业务 MQTT

从 2.1.48 起，业务客户端 `CabinetMqtt` 直接连接 `mqtt://iot.suji.work:1883`。在本地 `position_mqtt_local.h` 配置 Broker 凭证。业务连接默认取定位账号，可用 `JIUCHUAN_BUSINESS_MQTT_USERNAME` 与 `JIUCHUAN_BUSINESS_MQTT_PASSWORD` 配置独立业务账号。账号需要具备下方主题对应的发布或订阅权限。

业务主题中的 `{imei}` 为模组 IMEI 直接拼接 `s3`，与报文中的 `cabGuid` 相同。

| 用途 | 主题 |
| --- | --- |
| 命令和回执 | `topic/{imei}/cmd` |
| 事件 | `topic/{imei}/event` |
| 上线和状态上报 | `topic/{imei}/online` |
| 分组广播 | `topic/group/{groupID}` |
| 业务定位 | `topic/{imei}/position` |

## BLE 信标

ML307 的 AT 口就绪后，ESP32-S3 开始被动扫描 BLE 广播。MAC 包含 `D9:62:62` 且 RSSI 有效的记录会出现在串口和诊断页。串口示例为 `MAC=D9:62:62:00:00:0D type=public RSSI=-83 dBm`。

业务 MQTT 在 `topic/{imei}/online` 的 `KJ` 报文中用 `bleBeacons` 上报匹配结果，每个 MAC 保留最近一次 RSSI，最多 16 个。90 秒内无新采样的记录会过期。诊断页的 `KJ TX #44 BLE=1` 表示第 44 次本地发送包含 1 个信标。

```json
{"cmd":"KJ","bleBeacons":[{"macAddress":"D9:62:62:00:00:0D","rssi":-83}]}
```

ESP32 还会读取 USB RNDIS 的 IPv4 网关，从 RNDIS 接口地址每约 10 秒向该网关的 UDP `8000` 发送一份 BlueScan 快照。没有有效样本时发送 `[]`。ML307 用自己的定位数据合包上报。ESP32 在诊断页用 `BLUE WAIT IP` 显示等待网关，`BLUE WAIT OK` 显示等待回复，`BLUE OK 2` 表示网关确认收到 2 个信标，`BLUE NO ACK` 表示约 3 秒未取得 `OK` 将在后续周期重试。`BLUE OK` 确认的是模组收包，服务器到达情况以模组上报或服务端记录为准。

```json
[{"mac":"D9:62:62:00:00:0D","rssi":-83},{"mac":"D9:62:62:00:00:0E","rssi":-65}]
```

## 烧录

使用 ESP-IDF 5.5.4 和仓库根目录的 `sdkconfig.defaults`。完整 BIN 烧录地址为 `0x0`，串口监视器使用 115200 波特率。编译命令见[根目录说明](../../../README.md)。
