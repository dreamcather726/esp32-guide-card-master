# Vanex S3 导览卡项目

本仓库同步 ESP32-S3 固件、智控台前后端、语音服务及配套接口文档。保留客户仓库原有提交历史，固件仍位于仓库根目录，服务端位于 `server/`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `main/` | 固件业务、音频、屏幕、OTA、MQTT、ML307 和板级实现 |
| `main/boards/jiuchuan-s3/` | 当前 Vanex S3 板型、SD 内容管理、定位与电源管理 |
| `components/` | 必须保留的本地组件，包括 USB CDC/RNDIS 和电量估算修订 |
| `tests/` | 固件主机侧回归测试和源码检查 |
| `server/main/manager-api/` | Java 管理接口、角色、用户画像、设备绑定和对话记录 |
| `server/main/manager-web/` | Vue 智控台页面 |
| `server/main/xiaozhi-server/` | Python 实时语音服务 |
| `server/deploy/` | MQTT 绑定桥接、OTA、迁移、部署与诊断脚本 |
| `docs/customer-api/` | 客户接口说明和不含密钥的 Postman 集合 |

## 固件构建

使用 ESP-IDF **5.5.4** 的已初始化终端，目标为 ESP32-S3。首次构建会下载托管依赖；本地 `components/` 应与源码一起保留。

当前交付源码包含 **2.1.13-diag** 的诊断改动。`CMakeLists.txt` 默认版本为 `2.1.10`，复现诊断版本需显式传入版本号和配置，勿仅凭默认版本判断源码新旧。

PowerShell，在仓库根目录执行：

```powershell
$env:VANEX_SOURCE_DIR = (Get-Location).Path.Replace('\', '/')
idf.py -B build_crash_diag "-DPROJECT_VER=2.1.13-diag" "-DSDKCONFIG=sdkconfig.crash_diag" "-DSDKCONFIG_DEFAULTS=sdkconfig.jiuchuan-s3;sdkconfig.diagnostics" build
```

Linux/macOS 的已初始化 IDF 终端：

```sh
export VANEX_SOURCE_DIR="$PWD"
idf.py -B build_crash_diag -DPROJECT_VER=2.1.13-diag -DSDKCONFIG=sdkconfig.crash_diag '-DSDKCONFIG_DEFAULTS=sdkconfig.jiuchuan-s3;sdkconfig.diagnostics' build
```

`dependencies.lock` 保留依赖版本和哈希，三个本地组件路径通过 `VANEX_SOURCE_DIR` 解析，避免绑定开发者电脑路径。组件管理器构建时可能将它们展开为绝对路径；提交前恢复为环境变量形式即可，保留版本锁定。诊断构建产物位于 `build_crash_diag/`，本源码仓库不附带烧录包。

OTA 使用应用镜像，不使用整片合并烧录镜像；分区兼容性和升级流程见 [OTA 断点续传](docs/OTA_RESUMPTION.md) 与 [OTA 接口](docs/customer-api/OTA检查升级接口.md)。

## 服务端开发

### 管理 API

需要 Java 21、Maven、MySQL 和 Redis。数据库结构及增量脚本位于 `server/main/manager-api/src/main/resources/db/`，部署前先备份已有数据库并核对迁移配置。

```sh
cd server/main/manager-api
mvn -DskipTests=false test
mvn package
```

`application-dev.yml` 为开发配置，数据库密码使用环境变量 `MYSQL_ROOT_PASSWORD`；生产地址和凭证应通过外部 Spring 配置或环境变量提供。仓库中的 `CHANGE_ME` 仅为占位符。

### 智控台前端

使用 Node.js/npm，执行：

```sh
cd server/main/manager-web
npm ci
npm run build
node --test tests/device-user-binding.test.cjs tests/device-management.test.cjs
```

按部署环境设置 API 地址。不要在前端配置中放置集成 API Key、数据库密码或 MQTT 后台服务凭证。

### Python 语音服务

依赖清单推荐 Python 3.10，还需按原部署说明安装音频系统依赖及所选模型。以下模板用于从管理 API 获取模型配置的模式：

1. 进入 `server/main/xiaozhi-server/`，创建虚拟环境并安装 `requirements.txt`。
2. 将 `config.example.yaml` 复制为 `config.yaml`。
3. 创建 `data/`，将 `config_from_api.example.yaml` 复制为 `data/.config.yaml`。
4. 在本地配置中填写管理 API 地址及对应的 `server.secret`，配置智控台中的 ASR/LLM/TTS 等服务后执行 `python app.py`。

真实配置文件和模型缓存已由 Git 忽略。完整上游部署说明见 [服务端说明](server/README.md)。

### 部署脚本

`server/deploy/` 保留现有运维实现，其中历史脚本可能包含固定目录、日期和设备筛选条件。先阅读对应目录说明并调整环境，再执行；整仓上传并不会运行这些脚本或自动上线。

MQTT 绑定桥接配置参考 `server/deploy/mqtt-binding-bridge/bridge.env.example`，使用独立后台服务账号。历史 MQTT 网页示例仅作参考，已清空内置凭证；地图示例需自行配置地图 Key。

认证审计脚本 `server/deploy/device-integration/audit_customer_auth.py` 使用 `AUDIT_OLD_KEY_ID`、`AUDIT_AGENT_ID`、`AUDIT_USER_GUID`、`AUDIT_CREDENTIAL_FILE` 环境变量。按实际部署设置 `AUDIT_PILOT_DIR`，该路径需包含 `host_pilot.py`；不要提交凭证文件。

## 接口文档

从 [客户接口目录](docs/customer-api/README.md) 开始，包含：

- OTA 检查升级与下载、Range 断点续传。
- 用户画像新增/更新、查询及前缀匹配。
- 智能体角色/音色查询和媒体字段维护。
- 指定设备角色绑定、对话记录查询和清空。

业务接口使用所属账号的集成 API Key 或登录 Token。OTA 检查使用单独协议，详见专章。实际 API Key 单独交付，不放入 Git。

## 同步范围与检查

本次同步保留源码、资源、数据库迁移、配置模板、测试和文档；排除私钥、实际密码/Token、用户画像和对话数据、数据库备份、日志、依赖缓存、编译产物及烧录包。

同步时执行源码清单核对、凭证扫描、OTA Postman 离线检查、前端设备管理/用户绑定测试及源码语法检查。本次不代表重新完成固件全量编译、实机验证或服务器上线。

客户仓库既有历史保持不变；旧历史里曾提交过的凭证即使在当前版本清空，仍需由其所有者轮换。

## 上游与许可

保留根目录 `LICENSE`、`server/LICENSE` 及各组件许可。上游固件介绍保存在 [FIRMWARE_UPSTREAM.md](docs/FIRMWARE_UPSTREAM.md)，原中文介绍见 [README_zh.md](README_zh.md)。使用和分发时遵守相应许可及第三方资源要求。
