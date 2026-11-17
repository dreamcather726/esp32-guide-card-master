# 用户画像前缀查询接入说明

更新日期：2026-10-14

本次增加 userGUID 的“完整匹配或前缀匹配”查询，最多返回 30 条画像。
原有 credentials.json、API Key、设备固件保持不变，无需重新烧录。

## 查询方式

请求方法：GET，无需请求体。

```text
https://robot.suji.work/xiaozhi/user/profile/DTkxHempvRmp?match=prefix
```

请求头：

```http
X-API-Key: <credentials.json 中 apiKey 的完整值>
```

使用 X-API-Key 时不再添加 Authorization。原有智控台 Bearer Token 方式也支持前缀查询，两种认证方式选一种。

- 输入 DTkxHempvRmp，会查询该 ID 本身以及以它开头的 ID，例如 DTkxHempvRmp_01。
- 输入 DTkx，会查询以 DTkx 开头的 ID。无需添加星号或百分号。
- 这是前缀匹配，不是任意位置包含匹配；区分大小写。
- 下划线按普通字符匹配，不会当作通配符。
- 完整匹配优先返回，其余按 userGUID 升序排列；总计最多 30 条，超过后截断，可输入更长的前缀缩小范围。
- userGUID 不可留空；API Key 方式支持 1 至 128 位英文字母、数字、下划线、短横线。
- 只返回该认证账号所属的画像，不会跨账号查询；查询不会创建画像、绑定设备或改变使用次数。

## 返回格式

成功时 code=0，data 始终是数组；只有一条也返回数组，没有结果返回空数组 []。
每条记录包含 userGUID、profileVersion、profile，profile 中保留现有六个字段。

以下为结构示例，不代表实际用户数据：

```json
{
  "code": 0,
  "msg": "success",
  "data": [
    {
      "userGUID": "DTkxHempvRmp",
      "profileVersion": 2,
      "profile": {
        "nickname": "张先生",
        "language": "zh-CN",
        "interests": "历史文化",
        "explanationStyle": "普通话，简短讲解",
        "relationship": "我自己",
        "usageCount": 3
      }
    },
    {
      "userGUID": "DTkxHempvRmp_01",
      "profileVersion": 1,
      "profile": {
        "nickname": "李女士",
        "language": "zh-CN",
        "interests": "建筑",
        "explanationStyle": "通俗易懂",
        "relationship": "家人",
        "usageCount": 1
      }
    }
  ]
}
```

HTTP 200 本身不表示业务成功，请始终检查 JSON 中的 code。失败时读取 msg，不要将错误当成“没有匹配”。

## 截图中的测试工具如何调整

接口地址输入框仍填 https://robot.suji.work/xiaozhi/user/profile/，userGUID 输入框填完整 ID 或前缀。
工具的 GET 请求代码需增加查询参数，并把 data 按列表处理，而不是按单个画像对象处理。
仅在输入框填写较短 ID、请求仍未带 match=prefix 时，服务器依然执行原有精确查询。

若工具使用 Python requests，可以这样调用：

```python
import json
from urllib.parse import quote
import requests

with open("credentials.json", encoding="utf-8-sig") as f:
    credentials = json.load(f)

prefix = "DTkxHempvRmp"  # 换成 userGUID 输入框内容
url = "https://robot.suji.work/xiaozhi/user/profile/" + quote(prefix, safe="")
headers = {"X-API-Key": credentials["apiKey"]}
response = requests.get(url, params={"match": "prefix"}, headers=headers, timeout=20)
response.raise_for_status()
result = response.json()
if result.get("code") != 0:
    raise RuntimeError(result.get("msg", "查询失败"))
rows = result["data"]
for row in rows:
    print(row["userGUID"], row["profile"])
```

列表中选中一条记录后，使用返回的完整 userGUID 执行 PUT。
PUT 请求不要带 match=prefix，服务端会拒绝前缀更新，避免改错用户。
API Key 仅保存在客户后端或受保护的本地凭证文件中，勿写入公开仓库、网页代码或截图。

## 兼容规则

- GET /user/profile/{userGUID}?match=prefix：本次新增的列表查询。
- GET /user/profile/{userGUID}：保留旧精确查询，data 是单个对象或 null。
- GET /user/profile/{userGUID}?match=exact：显式精确查询，结果同旧接口。
- PUT /user/profile/{完整userGUID}：原新增、修改功能保持不变，不支持批量更新。
- 新查询不改变画像更新、下次绑定生效、使用次数由客户传入等原有规则。
- 无需更换 credentials.json，无需设备 OTA。
