# Vanex S3 OTA 检查升级接口

核验日期：2026-10-22。本说明依据现有固件、服务端代码及线上临时测试设备验证编写。
上一版客户文档遗漏了本接口。本次补齐资料，不修改线上认证规则，也不触发客户设备升级。

## 1. 三类操作的区别

| 操作 | 接口 | 含义 |
| --- | --- | --- |
| 检查设备升级 | `POST https://robot.suji.work/xiaozhi/ota/` | 上报设备 MAC、当前版本和板型，取得升级信息及设备配置 |
| 下载 OTA 应用 | `GET <检查结果中的 firmware.url>` | 获取应用二进制，支持 HTTP Range 续传 |
| 查看服务状态 | `GET https://robot.suji.work/xiaozhi/ota/` | 返回服务运行状态文本，不执行版本比较 |

**客户要检查升级，应使用第一项 POST，不是浏览器直接打开地址，也不是 `/integration` 下的角色接口。**

兼容旧地址：`http://47.94.91.32:8002/xiaozhi/ota/`，推荐使用 HTTPS 域名。
地址末尾的 `/` 按示例保留。设备发布版本当前为 `2.1.13-diag`，以后以接口返回为准。

## 2. 请求头及认证

OTA 是设备启动配置协议，与画像、角色接口的认证方式不同：

```http
Content-Type: application/json
Device-Id: <设备实际MAC，例如7c:4f:ad:24:82:04>
Client-Id: <设备UUID，可选>
```

| 请求头 | 必填 | 说明 |
| --- | --- | --- |
| Content-Type | 是 | application/json |
| Device-Id | 是 | 实际 MAC，建议使用设备上报的冒号分隔、小写形式 |
| Client-Id | 否 | 固件填设备 UUID；省略时服务器使用 Device-Id |

本设备 OTA 检查接口当前无需 `X-API-Key` 或智控台 Bearer Token，Postman 选择 **No Auth**。
下载接口凭临时 URL 中的票据访问，也不发送集成 API Key。
Device-Id 是设备标识，不是密码；此说明描述现有设备协议，不表示 OTA 管理和发布接口也免认证。

注意：角色/对话接口的 deviceId 可支持 IMEI/cabGuid 映射，**OTA 检查的 Device-Id 则要填 MAC**。
不要把 IMEI、cabGuid、userGUID、agentId、keyId 或智控台设备记录 ID 填到这里。
可从 `device/broadcast/online` 中的 `macAddress` 或开机日志获取 MAC。
HTTP Client-Id 也不是要求复用 MQTT clientId。

## 3. 最小请求体

```json
{
  "application": {
    "version": "2.1.12-diag"
  },
  "board": {
    "type": "jiuchuan-s3"
  }
}
```

- `application.version`：设备实际已安装版本，取开机日志或在线上报的 fwVersion；不是希望升级到的版本。
- `board.type`：当前 Vanex S3 固件对应 `jiuchuan-s3`，不是屏幕名称 `Vanex S3`、芯片名称 `esp32s3` 或智能体名。
- 省略 application、版本或板型不是有效的检查请求；按上面完整填写。
- 不需要传 userGUID、groupID、角色或 API Key 到请求体。

**此 POST 不是纯只读接口。** 对已登记设备，服务器会异步更新其上报版本和最近连接时间；
对未登记 MAC，会生成/复用激活信息。因此不要用真实设备 MAC 配上虚构旧版本反复测试。
接口在电脑上执行只返回配置，不会向远端设备发烧写命令。

## 4. 有升级时的返回

HTTP 200，以下为已脱敏的结构示例。此成功响应没有统一的 code/data 外壳：

```json
{
  "server_time": {
    "timestamp": 1792600000000,
    "timeZone": "Asia/Shanghai",
    "timezone_offset": 480
  },
  "firmware": {
    "version": "2.1.13-diag",
    "url": "https://robot.suji.work/xiaozhi/otaMag/download/临时票据"
  },
  "websocket": {
    "url": "设备连接地址，以实际返回为准",
    "token": "设备连接凭证，已脱敏"
  }
}
```

下载用 `firmware.url` 的实际完整地址；不要把上述中文占位符当作可下载地址。
该响应还可能包含 activation、mqtt 等配置。WebSocket/MQTT 凭证和下载票据应按敏感信息处理，勿公开完整响应。
当前 firmware JSON 只有 version、url；文件大小和 SHA-256 从下载响应头读取。

## 5. 正确判断升级

按以下顺序判断，不能只看 HTTP 200 或只看 firmware.version：

1. HTTP 非 200：按网络或 HTTP 错误处理，不执行升级。
2. JSON 含 `error`，或含非 0 的 `code`：按接口错误处理，记录 error/msg，不当作“已经最新”。
3. 含 activation：先检查智控台设备登记/激活状态。
4. 缺少 firmware 或 URL 为空：没有取得升级包，检查自动升级开关、板型及请求参数。
5. URL 含 `NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL`：这是兼容旧固件的无升级占位值，不要访问。
6. 返回有效下载 URL，且版本数字部分高于当前版本：本设备可以升级。

无升级占位 URL 当前为：

```text
http://xiaozhi.server.com:8002/xiaozhi/otaMag/download/NOT_ACTIVATED_FIRMWARE_THIS_IS_A_INVALID_URL
```

它不是部署地址配置错误，也不是另一台需要连接的 OTA 服务器。
版本比较按数字分段，不按字符串字典序；当前服务忽略 `-diag` 和构建后缀参与排序，
`2.1.13` 与 `2.1.13-diag` 视为同一数字版本，不反复升级；更高版本不自动降级。

| 情况 | 现有返回/处理 |
| --- | --- |
| 智控台已登记、自动升级开启、匹配板型、旧版本 | 新版 firmware.version 和有效临时 URL |
| 同版本或更高版本 | 无升级占位 URL；version 可能仍是服务器发布版本，单看它会误判 |
| 自动升级关闭 | 不返回 firmware |
| MAC 尚未在智控台登记 | activation 和无升级占位 URL |
| 其他板型 | 不下发 Vanex S3 固件；可能返回占位 URL |
| 用 IMEI/cabGuid 代替 MAC | HTTP 200，但 JSON 为 `{"error":"Invalid device ID"}` |
| 漏填 Device-Id | 本次实测 HTTP 200，JSON 为 `{"code":500,"msg":"服务器内部异常","data":null}` |

最后一项是当前服务端错误映射不够明确的问题，不是成功；本次文档补充未修改该服务行为。
所说“登记/激活”指设备加入智控台，与当次使用的 userGUID 绑定不同。
全量发布覆盖匹配且允许升级的设备，不会绕过设备登记、板型或自动升级关闭设置。

## 6. 完整下载与断点续传

### 完整下载

```http
GET <firmware.url>
Accept-Encoding: identity
```

正常返回 HTTP 200 和二进制内容，关键响应头如下：

```http
Content-Type: application/octet-stream
Content-Length: 3236640
Accept-Ranges: bytes
ETag: "4273f51f4c92ca0cb50d22d3a6705417fea9321c0bef8665d0367ef2215bfdee"
X-Firmware-SHA256: 4273f51f4c92ca0cb50d22d3a6705417fea9321c0bef8665d0367ef2215bfdee
```

上面的大小及哈希只对应当前 2.1.13-diag 应用包。校验对象是完整下载内容，不是 JSON、分片或完整烧录包。

### 中断后继续

假设已收到前 65536 字节，保留第一次响应的完整 ETag（含双引号）：

```http
GET <firmware.url>
Accept-Encoding: identity
Range: bytes=65536-
If-Range: "第一次响应中的ETag"
```

成功返回 HTTP 206，例如：

```http
Content-Range: bytes 65536-3236639/3236640
Content-Length: 3171104
```

- 206：核对起始偏移、总大小、ETag 和 SHA-256 后续接文件。
- 200：服务端返回了整包，通常是 If-Range 不匹配；从头写入，不能附加到旧分片后。
- 416：偏移越界或 Range 不合法；核对已有长度，完整文件需校验哈希，否则重新下载。
- 404：票据过期、固件记录或文件不存在；重新检查版本取得地址，仍失败再排查发布文件。
- 5xx：服务端异常；保留错误时间及状态，不把错误正文写成固件。

当前票据默认有效期为签发后 24 小时，多次分片读取不会消耗旧版“三次下载”额度，也不会延长票据寿命。
过期后重新取得地址；只有文件标识、大小和哈希仍相同才继续拼接，文件变化则重新下载。

当前固件自身实现 NVS 下载检查点，通常每 64 KiB 保存一次，恢复时验证已写前缀。
服务端支持 Range 不代表所有老固件都已具备掉电续传能力；设备仍需运行包含该实现的固件。
本次验证覆盖服务器下载协议，不替代真实设备断网、掉电及升级启动验收。

## 7. cURL 示例

下面是 Bash 示例。在自己业务服务器设好 DEVICE_MAC 和 CURRENT_VERSION 后运行：

```bash
curl --fail-with-body -X POST 'https://robot.suji.work/xiaozhi/ota/' \
  -H 'Content-Type: application/json' \
  -H "Device-Id: ${DEVICE_MAC}" \
  --data "{\"application\":{\"version\":\"${CURRENT_VERSION}\"},\"board\":{\"type\":\"jiuchuan-s3\"}}"
```

确认响应有效后，把真实 firmware.url 放到 OTA_URL，仅下载到本机文件：

```bash
curl --fail --show-error "$OTA_URL" -H 'Accept-Encoding: identity' \
  -D ota-headers.txt -o xiaozhi.bin
sha256sum xiaozhi.bin
```

下载到电脑不会升级 ESP32。设备需由自身 OTA 流程下载、校验、写入备用分区、切换启动并重启。
这里下载的是 OTA 应用 `xiaozhi.bin`，不是烧到 0x0 的合并镜像；不要在 OTA 下载接口放入 full.bin。

## 8. Postman 示例

导入本包 `ota-api.postman_collection.json`，与原有业务接口集合分开，避免继承 API Key。

- 填 `deviceMac`：实际 MAC。
- 填 `currentVersion`：设备当前真实版本。
- `boardType` 默认 `jiuchuan-s3`，`otaBaseUrl` 默认 `https://robot.suji.work/xiaozhi`。
- 执行“2 - Check device OTA”后，有升级时集合自动记录 otaDownloadUrl。
- 执行“3 - Download first 64 KiB”可验证 206，并保存 ETag 和下一偏移。
- 执行“4 - Download remaining bytes”验证续传响应；Postman 不会替你把两次响应拼成完整文件。
- 需要完整应用时用“5 - Download full OTA application”的 Send and Download 保存文件。

Postman 检查脚本会先清空旧下载地址，避免失败后误用上一次结果。
它不会发送设备重启或安装指令。下载 URL 为临时凭证，共享集合前清空该变量及 ETag。

## 9. 检查、发布与远程触发的边界

设备开机进入版本检查，前台失败时本固件有后台延迟重试逻辑；后台重试成功不等于永久周期轮询。
在电脑调用检查接口，不会让另一台已运行设备立刻 OTA。

服务端存在 OTA 固件管理接口 `/otaMag`，用于上传/发布，需要后台管理员权限；
它与本页设备检查接口、`X-API-Key` 业务集成接口是不同权限体系，客户集成密钥不含发布权限。
固件侧也有设备工具 `self.upgrade_firmware`，但它不等于本次交付的业务 API Key 已能远程控制升级，
需具备相应设备工具通路、账号权限、设备在线状态及兼容工具能力，调用接受也不代表安装成功。
本次没有新建 `/integration/ota/check` 或“一键推送”接口，没有更改管理员权限。

对于当前全量 OTA，设备保持联网、供电稳定，空闲时重启进入检查即可。
完成后以设备新的 fwVersion/开机日志确认，不能以 HTTP 下载完成或服务器已发布代替设备安装验收。

## 10. 本次核验结果

- HTTPS 域名和旧 IP 入口各核验 13 项实际响应，包括有升级、同版本、高版本、关闭升级、
  板型不匹配、未登记、错误 MAC、缺少请求头及 GET 状态检查。
- 两个下载入口各通过 10 项下载/Range 验证，包括整包、206、哈希、重复续传、If-Range 不匹配和 416。
- 发现并记录缺少 Device-Id 时 HTTP 200/code=500 的错误映射现状，未将其当作成功响应。
- 使用临时测试设备，已清理测试记录及票据；未修改客户设备数据、OTA 发布状态或触发实际升级。
- 当前发布：2.1.13-diag，应用 3,236,640 字节；实机稳定性和安装结果仍需设备验证。
- Postman 集合 JSON 和脚本已做本地检查，24 项离线行为测试通过；覆盖错误响应、
  同版本/高版本、过期结果清理、下载地址、分片身份及 200/206/416 的处理。
  这些测试使用 Postman API 模拟环境，不代表已在客户电脑的 Postman 界面或真实设备上验收。
