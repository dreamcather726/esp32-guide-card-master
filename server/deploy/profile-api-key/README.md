# 用户画像专用 API Key

## 客户接入

地址不变：`https://robot.suji.work/xiaozhi/user/profile/{userGUID}`。
GET 查询，PUT 新增或完整更新。DELETE 不在本次范围内。

请求头（二选一）：

```http
X-API-Key: <独立交付的画像专用密钥>
Content-Type: application/json
```

原 `Authorization: Bearer <智控台登录Token>` 仍可用。
不要在同一次请求中同时传两种认证头；API Key 不是 MQTT 密码，也不是绑定 Token。
API Key 只放在客户自己的服务端环境变量或密钥管理系统中，使用 HTTPS。
不要放在网址、网页脚本、固件、Git 仓库或公开文档中。

PUT 示例：

```json
{
  "nickname": "张先生",
  "language": "zh-CN",
  "interests": "历史文化、建筑、人文故事",
  "explanationStyle": "使用普通话，通俗易懂，每次简短讲解；用户追问时再展开。",
  "relationship": "我自己",
  "usageCount": 3
}
```

- `userGUID` 使用 1 至 128 位英文字母、数字、下划线或短横线，例如 `DTkxHempvRmp`。
- 这是一把账号级画像专用密钥，该账号下的多个 userGUID 共用；不按设备生成。
- 所属账号由密钥确定，JSON 或 URL 查询参数中的 ownerId 不会改变数据归属。
- PUT 原四字段是完整替换，遗漏会清空。2026-10-10 新增 `relationship`（最长 64 字符）和 `usageCount`（0 至 2147483647 的 JSON 整数）。
- 新增字段不传或传 null 保留原值；`relationship:""` 清空关系，`usageCount:0` 重置次数。新画像默认关系 null、次数 0。
- 使用次数由客户传入并按绝对值覆盖，后台不按绑定、对话或播放自动增加。关系是资料标签，不是 RoleID，也不改变 AI 的角色和称呼。
- GET 不存在的画像返回 `code=0, data=null`。完整新版接入说明见 `server/deploy/profile-fields/CUSTOMER_API.md`。
- HTTP 成功后仍应检查 JSON `code=0`。密钥错误、过期、停用、账号停用返回 HTTP 401。
- 同时传两种凭证返回 400；API Key 请求画像子路径或其它方法返回 403；认证数据库故障返回 503。
- 密钥默认没有自动到期时间，但管理员可以立即停用或更换，账号停用也立即失效。
- 画像更新在下次重新绑定该用户时生效；本接口不执行用户绑定，也不改变活动会话的画像快照。
- 此功能仅更新服务端，设备不用重新烧录。专用密钥不授予设备控制、会话历史、内部服务或后台管理权限。

## 管理员操作

在部署主机上通过 SSH 运行脚本。脚本复用现有受保护的数据库连接辅助程序；数据库仅存 SHA-256 摘要。

```sh
python3 host_admin.py issue --username CUSTOMER_ACCOUNT --label profile-integration --output /root/profile-key-delivery.json
python3 host_admin.py revoke --username CUSTOMER_ACCOUNT --key-id KEY_ID
```

签发文件包含明文密钥，权限为 600，终端只输出非敏感 key ID。通过受保护通道交付后删除服务器上的临时交付文件。
更换时先签发新密钥，客户端切换并验证成功后，再停用旧 key ID。

## 发布与验证

`host_admin.py deploy` 要求部署前后 SHA-256 一致，并检查 JAR 差异仅包含本次允许的类和迁移文件。
它备份原 JAR 和数据库，只重启 API 容器；健康检查失败会恢复原 JAR。
新增表为 `ai_profile_api_key`，旧代码忽略它，因此应用回退时保留该表和 Liquibase 记录，不恢复整个数据库。

`smoke_profile_key.py` 使用随机隔离账号、画像和密钥验证真实 HTTP/MySQL，结束时清理自己的测试数据。
可通过 `PROFILE_API_BASE=https://robot.suji.work/xiaozhi` 验证公网 HTTPS 入口。

本机房 Nginx 全局启用了 `proxy_cache`，会把 HEAD 转成 GET。`install_proxy.py` 为 HTTPS 和 8002
入口的 `/xiaozhi/user/profile/` 单独安装禁止缓存、保留方法的路由；备份配置、检查语法后热加载。
画像路径以外的路由保持原样。原配置和 SHA-256 保存在部署目录的 `proxy-backup`、`proxy-state.json`。

本机回归命令（排除依赖旧本地数据库配置的测试）：

```sh
mvn -DskipTests=false '-Dtest=*,!DeviceTest,!loginControllerTest,!MqttBindingMysqlTest' package
```
