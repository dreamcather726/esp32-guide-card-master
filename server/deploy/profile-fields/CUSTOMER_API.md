# 用户画像接口接入说明

更新日期：2026-10-10

本版新增“角色关系”和“使用次数”。接口地址、认证方式不变，继续使用之前交付的 `credentials.json`，无需更换密钥或烧录设备。

## 1. 接口地址

`https://robot.suji.work/xiaozhi/user/profile/{userGUID}`

实际示例：`https://robot.suji.work/xiaozhi/user/profile/DTkxHempvRmp`

- GET：查询用户画像，不需要请求体。
- PUT：新增或修改画像，没有画像则创建，已有则更新。
- userGUID 放在 URL 中；API Key 方式支持 1 至 128 位英文字母、数字、下划线、短横线。
- 删除画像接口暂未开放；解除设备绑定不等于删除画像。

## 2. 请求头

```http
X-API-Key: <credentials.json 中 apiKey 的完整值>
Content-Type: application/json
```

使用 X-API-Key 时去掉 Authorization 请求头。原 `Authorization: Bearer <智控台登录Token>` 仍可用，两种方式选一种。
专用密钥仅用于所属账号下的画像 GET/PUT；它不是 MQTT 凭证、数据库密码或设备绑定 Token。

## 3. PUT 完整示例

```json
{
  "nickname": "张先生",
  "language": "zh-CN",
  "interests": "历史文化、建筑、人文故事",
  "explanationStyle": "使用普通话，简短讲解，用户追问时再展开。",
  "relationship": "我自己",
  "usageCount": 3
}
```

| 字段 | 含义 | 类型与范围 |
| --- | --- | --- |
| nickname | 用户称呼 | 字符串，最长 64 |
| language | 语言 | 字符串，最长 32，例如 zh-CN |
| interests | 兴趣 | 字符串，最长 1024 |
| explanationStyle | 讲解风格 | 字符串，最长 512 |
| relationship | 角色关系标签 | 字符串，最长 64，例如“我自己”“老婆”“爸爸”，支持自定义 |
| usageCount | 使用次数 | JSON 整数，0 至 2147483647，不加引号 |

使用次数由客户系统传入：传 3 就保存为 3，再传 8 就保存为 8，重复传 8 仍然是 8。
后台不按绑定、启动、对话或播放自动累计，也不将它用作使用次数限制。

关系字段只是用户资料标签，不是设备 RoleID 或 Agent ID。填写“老婆”“爸爸”不会让 AI 自动扮演或称呼对方为相应角色。

## 4. 更新兼容规则

- 原四字段 nickname、language、interests、explanationStyle 保持原规则：PUT 时建议全部带齐，遗漏会清空对应旧值。
- 新字段 relationship、usageCount 不传或传 null 时，更新已有画像会保留原值，兼容旧四字段客户端。
- 清空关系：传 `"relationship": ""`。
- 重置次数：传 `"usageCount": 0`。
- 新画像未提供新字段时，relationship 为 null，usageCount 为 0。
- usageCount 不接受负数、小数、数字字符串、布尔值或超出范围的数值，例如 -1、1.5、"3"、true 都会返回错误。
- 所属账号由认证凭证确定，请求里添加 ownerId 不会改变画像归属；同一个 userGUID 在不同账号下彼此隔离。

## 5. 成功返回示例

GET 与 PUT 成功都返回以下结构，profileVersion 以实际返回为准：

```json
{
  "code": 0,
  "msg": "success",
  "data": {
    "userGUID": "DTkxHempvRmp",
    "profileVersion": 2,
    "profile": {
      "nickname": "张先生",
      "language": "zh-CN",
      "interests": "历史文化、建筑、人文故事",
      "explanationStyle": "使用普通话，简短讲解，用户追问时再展开。",
      "relationship": "我自己",
      "usageCount": 3
    }
  }
}
```

查询不存在的画像时返回 `code=0, data=null`。业务失败时 code 非 0；HTTP 200 本身不代表保存成功，务必检查 code。
凭证失效通常返回 HTTP 401；同时传两种认证方式返回 400；专用密钥访问超出范围的画像路径或方法返回 403。
字段校验失败时读取返回的 code/msg，不要只判断 HTTP 状态。

## 6. 与设备使用的关系

保存成功后，画像 GET 立即能读到新值。设备当前会话使用绑定时的画像快照，重新绑定该用户后才取得新快照。
本接口不执行设备绑定，也不授权控制设备、查询会话记录或修改 AI 角色。历史会话仍保留当时的数据；老会话的新字段可能为 null。
旧版智控台画像页面继续可用，不会因只提交四个旧字段而清空这两个新字段；本次新增字段通过接口维护。

## 7. Python 调用示例

```python
import os
import requests

url = "https://robot.suji.work/xiaozhi/user/profile/DTkxHempvRmp"
headers = {"X-API-Key": os.environ["VANEX_PROFILE_API_KEY"]}
profile = {
    "nickname": "张先生",
    "language": "zh-CN",
    "interests": "历史文化、建筑、人文故事",
    "explanationStyle": "使用普通话，简短讲解，追问时再展开。",
    "relationship": "我自己",
    "usageCount": 3
}
response = requests.put(url, headers=headers, json=profile, timeout=20)
response.raise_for_status()
result = response.json()
if result.get("code") != 0:
    raise RuntimeError(result.get("msg", "画像更新失败"))

response = requests.get(url, headers=headers, timeout=20)
response.raise_for_status()
result = response.json()
if result.get("code") != 0:
    raise RuntimeError(result.get("msg", "画像查询失败"))
print(result["data"])
```

在客户后端配置环境变量 VANEX_PROFILE_API_KEY，值取自密钥文件 apiKey。
只通过 HTTPS 调用；密钥放在服务端环境变量或密钥管理系统，不放进网址、网页前端、固件、公开群或代码仓库。
