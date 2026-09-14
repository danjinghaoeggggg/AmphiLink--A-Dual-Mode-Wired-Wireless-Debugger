# 更新日志

本文件记录 AmphiLink 的重要功能和行为变更。

## 2.0.0 - 2026-09-14

**以下内容为相对于 1.0.0 版本的主要变化。**

### 调试性能

- 将默认 SWD/JTAG 时钟由 1 MHz 提升到 10 MHz；上位机仍可通过标准
  CMSIS-DAP `DAP_SWJ_Clock` 命令或 OpenOCD `adapter speed` 覆盖默认值。
- 三套 ESP32-S3 构建配置均由调试优化改为性能优化，以降低寄存器级 SWD
  位操作关键路径的执行开销。

### 无线串口

- 新增独立 UART-over-TCP 服务，使用 TCP `4443` 和 GPIO16/GPIO17，默认配置为
  `115200 8N1`，支持最高 `921600` 波特率。
- 新增网页无线串口配置区域及 `GET/PUT /api/v1/serial` 接口；配置立即生效，
  设备复位后恢复默认值。

### 安全性与存储

- 将 Wi-Fi 配置从明文 `profiles_v1` 升级为认证加密的 `profiles_v2`：使用
  PBKDF2-HMAC-SHA-256 派生密钥，并使用 AES-256-GCM 加密完整配置载荷。
- 网络列表和普通配置响应不再返回密码；新增登录后按网络 ID 读取密码、凭据
  锁定与确认重置机制，并在前端缩短明文密码的保留时间。
- CSV 预置凭据改为通过加密 staging blob 导入，并支持导入过程中断电后的恢复。
- Flash 布局改为统一的 2 MiB 基线，保留实际使用的 NVS 分区、单一 `factory`
  应用分区和只读 `webpage_vfs` 分区；删除 OTA 及未使用的文件系统分区和接口。

### 硬件与构建

- 新增无 PSRAM、Quad PSRAM 80 MHz、Octal PSRAM 40 MHz 三套独立固件配置，
  覆盖常见 ESP32-S3 模组，并启用 Flash Quad/Octal 自动检测。
- 增加 ESP32-S3 模组选型、PSRAM 类型、2 MiB Flash 基线及 R16V GPIO47/48
  1.8 V 电压域限制的中英文说明。
- 新增 GitHub Actions 自动构建三套固件、生成烧录清单并更新 `latest` 预发布版本。

### API 与界面

- 配置网页增加无线串口、按需显示密码、开放网络、凭据锁定提示和凭据重置功能，
  并将品牌图标改为独立的 `/icon.svg` 静态资源。
- `PUT /api/v1/networks/{id}` 支持省略密码以保留原值；`POST /api/v1/connect`
  支持直接提交 `profile_id`，避免为了连接而向浏览器返回密码。
- 同步扩充中英文 README 与接口协议，记录 USB、TCP、UDP、HTTP 和无线串口行为。

### 升级注意事项

- 由于分区布局已经变化，从 1.0.0 升级时必须同时烧录 2 MiB 分区表、应用镜像
  和网页镜像，不应只更新应用分区，也不能混用不同硬件配置的发布文件。
- 旧版明文 `profiles_v1` 不会原地迁移；检测到后会清空 `wifi_nvs`，需要重新配置
  Wi-Fi。设备标识 NVS 保持独立。
- Wi-Fi 凭据加密不等同于完整设备安全：本版本仍未启用 HTTPS、Secure Boot 或
  Flash Encryption。

## 1.2.0 - 2026-9-14

### 硬件兼容性

- 新增无 PSRAM、Quad PSRAM 80 MHz、Octal PSRAM 40 MHz 三套 ESP32-S3
  构建配置；所有配置以 2 MiB Flash 为基线，容量更大的模组可直接使用。
- 三套配置均启用 Flash Quad/Octal 自动检测；Octal PSRAM 使用较保守的
  40 MHz 以增加时序余量。文档补充按模组容量后缀选择固件以及 R16V
  GPIO47/48 的 1.8 V 限制。

### 分区与存储

- 删除 OTA 布局以及未使用的 `vfs`、`storage`、`phy_init` 分区，改为单一
  `factory` 应用和只读 `webpage_vfs` 的 2 MiB 分区表。
- 保留三个 NVS 分区及应用的原偏移，网页分区移动到 `0x1E0000`；从旧布局
  升级时必须同步烧录新分区表和网页镜像。
- 精简 Flash 模块为实际使用的 NVS 和只读网页接口，删除未使用的文件系统 API、
  SPIFFS/wear-levelling 组件依赖及 BSP Flash 包装层。

### 验证

- 使用 ESP-IDF 6.0.2 从独立空构建目录通过 no-PSRAM、Quad-PSRAM 和
  Octal-PSRAM 三套完整构建；应用镜像均小于 1 MiB。

## 1.1.0 - 2026-08-29

### 安全性

- 将 Wi-Fi 配置存储升级为 `profiles_v2`。使用 100,000 次
  PBKDF2-HMAC-SHA-256 从管理密码、随机盐和 ESP32-S3 STA MAC 派生密钥，
  并使用 AES-256-GCM 对完整配置载荷进行认证加密。
- 每次写入生成新的 16 字节盐和 12 字节 nonce，使用 16 字节认证标签，
  并将格式头作为 AAD 参与认证；敏感密钥和明文缓冲区使用后显式清零。
- 网络列表和配置修改响应不再返回 Wi-Fi 密码。登录会话只能通过单项接口
  按网络 ID 读取密码，相关响应禁止缓存。
- 已保存网络可以通过 `profile_id` 直接连接，避免为了连接而将密码返回浏览器。

### 存储与迁移

- 检测到旧版明文 `profiles_v1` 时，擦除 `wifi_nvs` 并创建空的加密存储。
- CSV 预置凭据在首次启动时通过默认 NVS 中的加密 staging blob 转换为
  `profiles_v2`，并支持转换过程中断电后的恢复。
- 管理密码变更、设备 MAC 不匹配或 GCM 验证失败时保留原密文并进入锁定状态；
  Wi-Fi 管理器使用空凭据启动配置热点。
- 新增提交式 NVS key 删除和指定 NVS 分区擦除能力，用于 staging 清理和管理员
  明确确认后的凭据重置。

### API 与网页

- 新增独立无线串口 UART-over-TCP 服务（TCP `4443`），默认 `115200 8N1`，固定使用
  GPIO16/GPIO17，并提供 `GET/PUT /api/v1/serial` 配置接口。
- 网页增加无线串口配置区域，顶部品牌图标改为静态资源 `/icon.svg`。
- 网络响应新增 `has_password` 和顶层 `locked` 字段；锁定状态返回修订号 `0`
  和空列表。
- 新增 `GET /api/v1/networks/{id}/password`，用于登录后按需读取单个密码。
- 新增 `POST /api/v1/networks/reset`，仅在提交 `{"confirm":true}` 时清空凭据。
- `PUT /api/v1/networks/{id}` 支持省略 `password` 以保留原密码，显式空字符串
  用于切换为开放网络。
- `POST /api/v1/connect` 新增 `{"profile_id":id}` 请求形式。
- 网页增加按需显示密码、开放网络选项、锁定提示和二次确认重置；隐藏密码、
  刷新、退出、关闭编辑框或离开页面时会清理 JavaScript 状态和输入框中的明文。
- 同步更新中英文 README 和协议文档。

### 验证

- 通过 ESP-IDF 6.0.2 全量构建。
- 通过固定密码学向量、错误密码和不同 MAC、加密数据篡改、序列化边界及随机
  nonce 测试。
- 通过网页 JavaScript 语法检查，以及中英文桌面和移动端布局检查。
- 真机刷写、实际掉电迁移和导出 `wifi_nvs` 后的明文扫描仍需在 ESP32-S3
  硬件上完成。

### 安全边界

- 此变更保护单独提取的 `wifi_nvs` 数据，并限制外部接口读取密码。
- 本项目仍未启用 HTTPS、Secure Boot 或 Flash Encryption；HTTP 流量监听、
  运行时内存读取，以及结合未加密固件进行逆向分析不在本次防护范围内。

## 1.0.0 - 2026-08-22

### 首次发布

- 提供基于 ESP32-S3 的 CMSIS-DAP V2 调试器，支持原生 USB 有线模式和
  OpenOCD CMSIS-DAP TCP 无线模式。
- 支持 SWD 调试、USB/TCP 传输路径选择、局域网设备发现，以及设备名称和
  颜色标识配置。
- 提供中英文 Wi-Fi 配置网页，可扫描、保存、排序、连接和删除网络。
- 支持通过 NVS 分区持久化 Wi-Fi 配置与设备标识，并可使用 CSV 镜像预置。
