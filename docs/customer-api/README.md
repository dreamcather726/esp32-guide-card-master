# Vanex S3 客户接口文档

整理日期：2026-10-22，第二版，已补齐 OTA。本文档包不包含真实密钥、数据库密码或内部部署文件。
本次为接口资料整理，没有修改服务器接口、设备设置或用户数据。

**OTA 检查升级请先看 [OTA检查升级接口](OTA检查升级接口.md)。**
使用 `POST https://robot.suji.work/xiaozhi/ota/`，Device-Id 填 MAC，No Auth。
该协议的成功响应直接包含 firmware，不适用下面业务接口的 API Key 和 code/data 规则。

## 接入步骤

本节仅针对用户画像及 integration 业务接口；OTA 使用上方单独说明。

1. 向项目负责人单独获取 `customer-api-key.json` 集成接口凭证文件。
2. 将文件中完整的 `apiKey` 配置在客户业务服务器，请求头使用 `X-API-Key`。
3. `keyId` 只是密钥标识，不是请求凭证；MQTT 临时凭证也不用于这些 HTTP 接口。
4. 使用 X-API-Key 时不要同时发送 Authorization。也可单独使用智控台账号登录 Token。
5. 先调用角色查询或画像 GET 验证接入。先检查 HTTP 状态，再检查 JSON `code=0`。

```http
X-API-Key: <customer-api-key.json 中完整的 apiKey>
Content-Type: application/json
```

密钥只访问其所属智控台账号的数据。新集成密钥应包含 profile、roles:read、roles:write、
device:role、history:read、history:delete 权限；旧的仅 profile 权限密钥仍只用于画像。
长期密钥放在客户服务端环境变量或密钥管理系统，不放进网页前端、固件、公开仓库或截图。

## 文档目录

- [OTA检查升级接口](OTA检查升级接口.md)：请求格式、MAC、版本选择、鉴权区别、下载及断点续传、错误处理。
- [OTA Postman 示例](ota-api.postman_collection.json)：5 个 OTA 状态/检查/下载请求，No Auth。
- [角色与设备对话接口](角色与设备对话接口.md)：角色/音色查询、媒体地址维护、设备角色绑定、对话查询与清空。
- [用户画像接口](用户画像接口.md)：画像 GET/PUT、六个字段、使用次数、更新兼容规则。
- [用户画像前缀查询](用户画像前缀查询.md)：按 userGUID 完整或前缀匹配，最多 30 条。
- [Postman 示例](vanex-api.postman_collection.json)：导入后填写变量使用，默认不含密钥。

## 接口总览

下表路径统一以 `https://robot.suji.work/xiaozhi` 为前缀。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/ota/` | 设备 OTA 检查升级，使用 MAC、当前版本和板型 |
| GET | `/ota/` | 仅查看服务状态，返回文本 |
| GET | `/otaMag/download/{临时票据}` | 下载检查接口返回的应用，支持 Range |
| GET | `/user/profile/{userGUID}` | 精确查询画像，data 为对象或 null |
| GET | `/user/profile/{userGUID}?match=prefix` | 前缀查询画像，data 为数组，最多 30 条 |
| PUT | `/user/profile/{userGUID}` | 新增或修改单个用户画像 |
| GET | `/integration/agents/{agentId}/roles` | 查询智能体全部角色及对应音色 |
| PUT | `/integration/agents/{agentId}/roles/{roleId}/media` | 更新图片、视频、试听音频 URL |
| GET | `/integration/devices/{deviceId}/role-binding` | 查询设备保存的角色绑定 |
| PUT | `/integration/devices/{deviceId}/role-binding` | 设置指定设备的角色 |
| GET | `/integration/devices/{deviceId}/chat-history?page=1&limit=30` | 分页查询设备对话 |
| DELETE | `/integration/devices/{deviceId}/chat-history?confirm=true` | 清空指定设备对话 |

## Postman 使用

业务接口导入 `vanex-api.postman_collection.json` 后填写下表变量。
OTA 另导入 `ota-api.postman_collection.json`，填写 deviceMac 和 currentVersion，详见 OTA 专章；不要继承业务接口 API Key。

| 变量 | 填写内容 |
| --- | --- |
| apiKey | 私下交付的集成凭证文件中的完整 apiKey |
| agentId | 当前账号下的真实智能体 ID |
| deviceId | 智控台设备 ID、MAC，或已登记映射的 IMEI/cabGuid |
| roleId | 角色查询接口返回的 roleId |
| userGUID | 用户完整 ID；前缀查询时也可填前缀 |

`baseUrl` 为集成接口地址，`profileBaseUrl` 为画像接口地址，保留默认即可。
共享或导出集合前清空 apiKey。集合包含写入和删除请求，请逐条执行，勿直接运行整个集合。
执行 PUT 前核对目标及请求体；执行 DELETE 前二次确认设备，操作不可撤销且会中断其在线语音连接。

## 常见问题

- **OTA 怎么检查**：POST /xiaozhi/ota/，Device-Id 用 MAC，版本放 application.version，板型放 board.type。
  GET 同地址仅返回服务状态；用电脑调用检查并不会向设备推送升级。
- **401**：检查是否使用完整 apiKey、凭证是否有效，以及是否包含接口所需权限。
  `Invalid API key or operation outside key scope` 也可能表示权限不足，不只表示过期。
- **画像保存后设备仍使用旧话术**：保存后 GET 可立即查到；设备需重新绑定该用户以加载新画像快照。
- **角色绑定成功但设备未立即切换**：检查 `data.runtimeStatus`；保存成功与在线生效是不同状态。
- **用户绑定和角色绑定**：本包的 role-binding 是给设备选角色，不负责关联 userGUID，也不转移设备所属智能体。
- **删除画像**：当前暂未开放；清空对话、解绑设备和删除画像是不同操作。
- **使用次数**：由客户传入 usageCount 保存，不按开机、绑定或对话自动累计。
- **是否需要烧录**：调用这些服务端接口无需为接口认证单独烧录或 OTA。

示例中的 example.com、DEVICE_ID、AGENT_ID、ROLE_ID 等均需替换为实际值。
