# AmphiLink 接口协议 / Interface Protocol

[中文](#中文) | [English](#english)

<a id="中文"></a>
## 中文

本文档描述 AmphiLink 固件对外提供的设备发现、CMSIS-DAP 传输和网页配置接口，供调试工具、设备管理工具及其他社区客户端实现者参考。除特别说明外，多字节整数均采用小端字节序，字符串和 JSON 文本均使用 UTF-8 编码。

当前固件提供以下接口：

| 接口 | 传输方式 | 默认端口或端点 | 用途 |
| --- | --- | --- | --- |
| 设备发现 | UDP/IPv4 广播 | `4442` | 在局域网内发现在线设备 |
| 无线调试 | TCP/IPv4 | `4441` | 承载 CMSIS-DAP v2 请求和响应 |
| 有线调试 | USB 2.0 Vendor Bulk | OUT `0x01` / IN `0x81` | 承载 CMSIS-DAP v2 请求和响应 |
| 配置服务 | HTTP/1.1 | `80` | 管理设备标识和 Wi-Fi 配置 |

### UDP 广播协议

#### 工作方式

设备以 Wi-Fi STA 模式接入网络并取得 IP 地址后，每 `2000 ms` 向 `255.255.255.255:4442` 发送一个 UDP 广播数据报。设备离线、正在连接 Wi-Fi 或处于配置热点模式时不发送该广播。

客户端应从 UDP 数据报的源地址取得设备 IP，不应依赖 JSON 负载携带网络地址。发现设备后，客户端可使用该源地址和固定 TCP 端口 `4441` 建立调试连接。

#### 数据格式

每个 UDP 数据报仅包含一个无结尾空字节的 UTF-8 JSON 对象：

```json
{"name":"AmphiLink","id":1}
```

| 字段 | 类型 | 约束 | 说明 |
| --- | --- | --- | --- |
| `name` | string | 1-32 个可打印 ASCII 字符（`0x20`-`0x7e`） | 用户配置的设备名称 |
| `id` | integer | `1`-`10` | 设备颜色编号，用于区分同名或相邻设备 |

UDP 发现不包含请求、确认、重传或版本协商机制。客户端应将广播视为设备在线状态的周期性快照，并自行设置过期时间；建议连续丢失至少 2-3 个广播周期后再将设备标记为离线。

### TCP 调试接口

#### 连接模型

设备在 Wi-Fi STA 已连接时监听 TCP `4441`，同一时间只处理一个客户端连接。建议启用低延迟发送并按请求顺序等待响应。连接断开、Wi-Fi 断开或收到非法帧后，设备会清空当前解析状态并重置 CMSIS-DAP 核心会话；客户端必须重新建立连接。

TCP 是字节流协议。一个帧可以被拆分到多次发送中，多个帧也可以合并发送。客户端不得假设一次 `send` 或 `recv` 恰好对应一个完整帧。

#### 帧格式

请求和响应使用相同的 8 字节帧头，随后紧跟 CMSIS-DAP 负载：

| 偏移 | 长度 | 字段 | 取值或说明 |
| ---: | ---: | --- | --- |
| `0` | 4 | Signature | 固定为 `44 41 50 00`，即 ASCII `DAP\0` |
| `4` | 2 | Payload Length | 无符号小端整数，范围 `1`-`64` |
| `6` | 1 | Packet Type | 请求为 `0x01`，响应为 `0x02` |
| `7` | 1 | Reserved | 固定为 `0x00` |
| `8` | 可变 | Payload | 原始 CMSIS-DAP v2 命令或响应 |

请求帧示例（`DAP_Info`，查询 Vendor ID）：

```text
44 41 50 00 02 00 01 00  00 01
| signature | len |ty|rs|  payload
```

对应响应的帧头中 `Packet Type` 为 `0x02`，`Payload Length` 等于实际 CMSIS-DAP 响应长度。协议没有事务编号；响应与请求严格按顺序对应。CMSIS-DAP 负载的命令编号和字段定义遵循 Arm CMSIS-DAP v2 规范。

#### 帧校验与错误处理

设备按以下规则校验请求：

- Signature 必须为 `DAP\0`。
- Packet Type 必须为 `0x01`。
- Reserved 必须为 `0x00`。
- Payload Length 必须在 `1`-`64` 范围内。

任一条件不满足时，当前连接将被关闭，且不会返回协议层错误帧。客户端不应在 TCP 帧之间加入填充字节，也不应发送大于 64 字节的 CMSIS-DAP 负载。

固件支持最多 8 个 CMSIS-DAP 包的内部队列，并支持标准 `DAP_QueueCommands`、`DAP_ExecuteCommands` 和 `DAP_TransferAbort` 语义。队列容量是固件实现细节，客户端仍应进行合理的流量控制，并及时读取响应。

### USB 调试接口

#### 设备标识

| 属性 | 当前值 |
| --- | --- |
| USB 版本 | `2.10` |
| VID | `0x303A` |
| PID | `0x83B3` |
| 设备版本 | `1.00` |
| Manufacturer | `danjinghaoeggggg` |
| Product | `AmphiLink (CMSIS-DAP V2)` |
| Serial Number | ESP32-S3 基础 MAC，格式为 12 位大写十六进制字符串 |

USB 配置包含一个 Vendor Class 接口。Windows 主机可通过 Microsoft OS 2.0 描述符将该接口绑定到 WinUSB；接口 GUID 为 `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`。

#### 端点与数据格式

| 方向 | 端点 | 类型 | 最大包长 |
| --- | --- | --- | --- |
| Host -> Device | `0x01` | Bulk OUT | 64 字节 |
| Device -> Host | `0x81` | Bulk IN | 64 字节 |

USB 传输直接承载 1-64 字节的 CMSIS-DAP v2 负载，不使用 TCP 接口中的 `DAP\0` 帧头。每个 OUT 包是一条 CMSIS-DAP 请求，每个 IN 包是对应响应；命令和响应格式遵循 Arm CMSIS-DAP v2 规范。

主机应通过 CMSIS-DAP v2 的 Vendor Bulk 接口进行枚举和通信。USB 断开时，固件会重置 CMSIS-DAP 核心会话并丢弃尚未完成的会话状态。

> `VID`/`PID` 是当前固件配置。发布或销售衍生硬件前，维护者必须确认其有权使用相应 USB 标识，并在需要时替换描述符。

### 配置网页接口

#### 通用约定

配置服务监听 HTTP `80`。设备处于配置热点模式时，服务地址通常为 `http://192.168.4.1/`；设备作为 STA 接入局域网后，使用 `http://<device-ip>/`。

API 基础路径为 `/api/v1`，请求和响应正文均为 JSON。响应使用 `Content-Type: application/json; charset=utf-8` 和 `Cache-Control: no-store`。请求正文不得超过 512 字节。

除 `POST /api/v1/login` 外，所有 API 都要求携带登录接口返回的 `amphilink_auth` Cookie。该 Cookie 带有 `HttpOnly`、`SameSite=Strict` 和 `Path=/` 属性，并在配置服务重启后失效。

登录密码与配置热点密码相同。默认开发配置为 `12345678`，实际部署可能不同，客户端不应写死该值。

#### 登录

##### `POST /api/v1/login`

请求：

```json
{"password":"12345678"}
```

成功响应为 `200 OK`，同时通过 `Set-Cookie` 建立会话：

```json
{"authenticated":true}
```

密码错误返回 `401 Unauthorized` 和错误码 `invalid_password`。

#### 状态与设备标识

##### `GET /api/v1/status`

返回当前网络和调试连接状态：

```json
{
  "mode":"sta",
  "ssid":"Example-WiFi",
  "ip":"192.168.1.42",
  "ap_ssid":"AmphiLink-A1B2C3",
  "scanning":false,
  "dap_client":false
}
```

`mode` 的可能值为 `starting`、`connecting`、`sta` 和 `ap`。`dap_client` 表示当前是否存在 TCP 调试客户端。

##### `GET /api/v1/device`

返回设备标识：

```json
{"name":"AmphiLink","id":1}
```

##### `PUT /api/v1/device`

更新并返回设备标识：

```json
{"name":"Bench-A","id":6}
```

`name` 必须为 1-32 个可打印 ASCII 字符，`id` 必须为 `1`-`10` 的整数。修改成功后，UDP 广播内容和 WS2812 指示颜色会同步更新。

#### 已保存网络

设备最多保存 10 个 Wi-Fi 配置。网络列表响应统一使用以下结构：

```json
{
  "revision":7,
  "locked":false,
  "items":[
    {
      "id":3,
      "ssid":"Example-WiFi",
      "has_password":true,
      "priority":0,
      "connected":true
    }
  ]
}
```

`priority` 从 `0` 开始，数值越小，自动连接优先级越高。`id` 是稳定的配置标识，不等同于数组下标。列表和修改响应不会返回密码；`has_password` 仅表示该网络是否需要密码。`locked` 表示当前管理密码无法解密已保存凭据，此时版本为 `0` 且列表为空。

所有修改网络列表的请求都必须提交最近一次读取到的 `revision`。修改成功后响应包含递增后的版本；版本不匹配时返回 `409 Conflict` 和 `revision_conflict`，客户端应重新获取列表再合并用户操作。新增重复 SSID 或将现有配置改为另一个已保存的 SSID 时，当前固件也返回 `revision_conflict`。

SSID 必须是 1-32 字节的合法 UTF-8 字符串。密码为空字符串时表示开放网络；非空密码必须为 8-63 字节。

| 方法与路径 | 请求正文 | 成功状态 | 说明 |
| --- | --- | --- | --- |
| `GET /api/v1/networks` | 无 | `200 OK` | 获取网络列表 |
| `GET /api/v1/networks/{id}/password` | 无 | `200 OK` | 获取单个网络的密码 |
| `POST /api/v1/networks` | `{"revision":7,"ssid":"...","password":"..."}` | `201 Created` | 新增网络 |
| `PUT /api/v1/networks/{id}` | `{"revision":7,"ssid":"...","password":"..."}` | `200 OK` | 修改网络；省略密码表示保留原密码 |
| `DELETE /api/v1/networks/{id}` | `{"revision":7}` | `200 OK` | 删除指定网络 |
| `PUT /api/v1/networks/order` | `{"revision":7,"ids":[3,1,2]}` | `200 OK` | 按 ID 数组重排全部网络 |
| `POST /api/v1/networks/reset` | `{"confirm":true}` | `200 OK` | 擦除全部凭据并解除锁定 |

单项密码接口返回`{"password":"..."}`；开放网络返回空字符串。该接口与表中的其他网络接口都要求有效登录Cookie，并使用`Cache-Control: no-store`。重置接口用于`locked`状态，只有显式提交`confirm:true`才会擦除原密文并创建空的`profiles_v2`存储。

重排请求中的 `ids` 必须完整、无重复地列出当前全部网络 ID。

#### Wi-Fi 扫描

扫描采用异步操作模型。

1. 调用 `POST /api/v1/scan`。成功时返回 `202 Accepted`：

   ```json
   {"operation_id":12}
   ```

2. 轮询 `GET /api/v1/scan/12`，直至 `state` 为 `succeeded` 或 `failed`：

   ```json
   {
     "operation_id":12,
     "state":"succeeded",
     "error":0,
     "items":[
       {
         "ssid":"Example-WiFi",
         "rssi":-48,
         "channel":6,
         "auth":"wpa2",
         "supported":true,
         "saved":true
       }
     ]
   }
   ```

`state` 的可能值为 `idle`、`running`、`succeeded` 和 `failed`。`auth` 的可能值为 `open`、`wpa2`、`wpa3`、`wpa2_wpa3`、`legacy_wpa` 和 `unsupported`。存在活动 TCP 调试客户端时，固件拒绝启动扫描并返回 `dap_client_active`，以避免中断调试链路。

#### 切换网络

切换网络同样采用异步操作模型。

1. 调用 `POST /api/v1/connect`：

   ```json
   {"ssid":"Example-WiFi","password":"example-password"}
   ```

   已保存网络应改用 `{"profile_id":3}`，避免把密码返回给客户端。成功接受操作时返回 `202 Accepted` 和 `operation_id`。该调用会主动断开当前 TCP 调试客户端。

2. 在网络仍可访问时轮询 `GET /api/v1/connect/{operation_id}`：

   ```json
   {
     "operation_id":13,
     "state":"succeeded",
     "error":0,
     "ssid":"Example-WiFi"
   }
   ```

成功连接的配置会被保存并提升为最高优先级。切换网络可能改变设备 IP，客户端应预期当前 HTTP 连接中断，并通过 UDP 广播或 DHCP 信息重新发现设备。

#### 错误响应

所有 API 错误均使用统一结构：

```json
{"error":{"code":"invalid_profile"}}
```

常见错误码如下：

| HTTP 状态 | `code` | 含义 |
| --- | --- | --- |
| `400 Bad Request` | `invalid_json` | JSON 缺失、类型错误或字段不合法 |
| `400 Bad Request` | `invalid_identity` | 设备名称或颜色编号不合法 |
| `400 Bad Request` | `invalid_profile` | SSID 或密码不合法 |
| `400 Bad Request` | `invalid_size` | 字段或正文长度不合法 |
| `400 Bad Request` | `invalid_id` | 路径中的操作 ID 不合法 |
| `401 Unauthorized` | `invalid_password` | 登录密码错误 |
| `401 Unauthorized` | `unauthorized` | 未提供有效会话 Cookie |
| `423 Locked` | `credentials_locked` | 当前管理密码无法解密已保存的 Wi-Fi 凭据 |
| `404 Not Found` | `not_found` | 配置项、异步操作或资源不存在 |
| `409 Conflict` | `revision_conflict` | 网络列表版本冲突 |
| `409 Conflict` | `profile_limit` | 已达到 10 个网络的上限 |
| `409 Conflict` | `dap_client_active` | 活动调试连接阻止扫描 |
| `409 Conflict` | `operation_busy` | 另一个 Wi-Fi 操作正在执行 |
| `500 Internal Server Error` | `storage_error` | 持久化存储失败 |
| `503 Service Unavailable` | `not_ready` | 相关子系统尚未就绪 |
| `503 Service Unavailable` | `web_assets_missing` | 配置网页静态资源不可用 |

异步结果中的 `error` 是底层 ESP-IDF `esp_err_t` 数值：`0` 表示成功，非零值仅用于诊断。客户端的主要状态判断应以 `state` 和 HTTP 错误码为准，不应将具体数值视为跨固件版本稳定的公共枚举。

<a id="english"></a>
## English

This document defines the device discovery, CMSIS-DAP transport, and web configuration interfaces exposed by the AmphiLink firmware. It is intended for developers of debugging tools, device management tools, and other community clients. Unless otherwise specified, multibyte integers use little-endian byte order, and strings and JSON text use UTF-8 encoding.

The current firmware provides the following interfaces:

| Interface | Transport | Default port or endpoint | Purpose |
| --- | --- | --- | --- |
| Device discovery | UDP/IPv4 broadcast | `4442` | Discover online devices on the local network |
| Wireless debugging | TCP/IPv4 | `4441` | Carry CMSIS-DAP v2 requests and responses |
| Wired debugging | USB 2.0 Vendor Bulk | OUT `0x01` / IN `0x81` | Carry CMSIS-DAP v2 requests and responses |
| Configuration service | HTTP/1.1 | `80` | Manage device identity and Wi-Fi configuration |

### UDP Broadcast Protocol

#### Operation

After the device joins a network in Wi-Fi STA mode and obtains an IP address, it sends a UDP broadcast datagram to `255.255.255.255:4442` every `2000 ms`. No broadcast is sent while the device is offline, connecting to Wi-Fi, or operating as a configuration access point.

Clients must obtain the device IP address from the source address of the UDP datagram and must not expect the JSON payload to contain a network address. After discovering a device, a client can connect to that source address on the fixed TCP port `4441` for debugging.

#### Data Format

Each UDP datagram contains exactly one UTF-8 JSON object without a trailing null byte:

```json
{"name":"AmphiLink","id":1}
```

| Field | Type | Constraints | Description |
| --- | --- | --- | --- |
| `name` | string | 1-32 printable ASCII characters (`0x20`-`0x7e`) | User-configured device name |
| `id` | integer | `1`-`10` | Device color identifier used to distinguish devices with the same name or devices located near each other |

UDP discovery has no request, acknowledgement, retransmission, or version negotiation mechanism. Clients should treat each broadcast as a periodic snapshot of device availability and apply their own expiration policy. A device should preferably be marked offline only after at least two or three consecutive broadcast intervals have elapsed without a datagram.

### TCP Debug Interface

#### Connection Model

The device listens on TCP port `4441` while Wi-Fi STA is connected and accepts only one client connection at a time. Clients should use low-latency transmission and wait for responses in request order. When the TCP connection or Wi-Fi connection is lost, or when an invalid frame is received, the device clears its parser state and resets the CMSIS-DAP core session. The client must then establish a new connection.

TCP is a byte-stream protocol. A frame may be split across multiple sends, and multiple frames may be combined in a single send. A client must not assume that one `send` or `recv` call corresponds to exactly one complete frame.

#### Frame Format

Requests and responses use the same 8-byte header followed immediately by the CMSIS-DAP payload:

| Offset | Length | Field | Value or description |
| ---: | ---: | --- | --- |
| `0` | 4 | Signature | Fixed value `44 41 50 00`, which is ASCII `DAP\0` |
| `4` | 2 | Payload Length | Unsigned little-endian integer in the range `1`-`64` |
| `6` | 1 | Packet Type | `0x01` for a request and `0x02` for a response |
| `7` | 1 | Reserved | Fixed value `0x00` |
| `8` | Variable | Payload | Raw CMSIS-DAP v2 command or response |

Example request frame (`DAP_Info`, querying the Vendor ID):

```text
44 41 50 00 02 00 01 00  00 01
| signature | len |ty|rs|  payload
```

In the corresponding response header, `Packet Type` is `0x02`, and `Payload Length` is the actual length of the CMSIS-DAP response. The protocol has no transaction identifier; responses correspond to requests strictly in order. CMSIS-DAP payload command identifiers and fields follow the Arm CMSIS-DAP v2 specification.

#### Frame Validation and Error Handling

The device validates requests according to the following rules:

- Signature must be `DAP\0`.
- Packet Type must be `0x01`.
- Reserved must be `0x00`.
- Payload Length must be in the range `1`-`64`.

If any condition is not met, the current connection is closed without a protocol-level error frame. Clients must not insert padding bytes between TCP frames or send CMSIS-DAP payloads larger than 64 bytes.

The firmware provides an internal queue for up to eight CMSIS-DAP packets and implements the standard `DAP_QueueCommands`, `DAP_ExecuteCommands`, and `DAP_TransferAbort` semantics. Queue capacity is a firmware implementation detail; clients should still apply reasonable flow control and read responses promptly.

### USB Debug Interface

#### Device Identification

| Property | Current value |
| --- | --- |
| USB version | `2.10` |
| VID | `0x303A` |
| PID | `0x83B3` |
| Device version | `1.00` |
| Manufacturer | `danjinghaoeggggg` |
| Product | `AmphiLink (CMSIS-DAP V2)` |
| Serial Number | ESP32-S3 base MAC address formatted as 12 uppercase hexadecimal characters |

The USB configuration contains one Vendor Class interface. On Windows, the interface can be bound to WinUSB through the Microsoft OS 2.0 descriptor. Its interface GUID is `{CDB3B5AD-293B-4663-AA36-1AAE46463776}`.

#### Endpoints and Data Format

| Direction | Endpoint | Type | Maximum packet size |
| --- | --- | --- | --- |
| Host -> Device | `0x01` | Bulk OUT | 64 bytes |
| Device -> Host | `0x81` | Bulk IN | 64 bytes |

USB transfers carry CMSIS-DAP v2 payloads of 1-64 bytes directly and do not use the `DAP\0` header defined for the TCP interface. Each OUT packet is one CMSIS-DAP request, and each IN packet is the corresponding response. Commands and responses follow the Arm CMSIS-DAP v2 specification.

The host should enumerate and communicate with the CMSIS-DAP v2 Vendor Bulk interface. When USB disconnects, the firmware resets the CMSIS-DAP core session and discards any incomplete session state.

> The `VID` and `PID` are values used by the current firmware configuration. Before distributing or selling derivative hardware, maintainers must confirm that they are authorized to use these USB identifiers and replace the descriptors when required.

### Configuration Web Interface

#### General Conventions

The configuration service listens on HTTP port `80`. While the device is operating as a configuration access point, the service is normally available at `http://192.168.4.1/`. After the device joins a local network as a STA, use `http://<device-ip>/`.

The API base path is `/api/v1`. Request and response bodies use JSON. Responses include `Content-Type: application/json; charset=utf-8` and `Cache-Control: no-store`. A request body must not exceed 512 bytes.

Except for `POST /api/v1/login`, every API request must include the `amphilink_auth` cookie returned by the login endpoint. The cookie has the `HttpOnly`, `SameSite=Strict`, and `Path=/` attributes and becomes invalid when the configuration service restarts.

The login password is the same as the configuration access-point password. The default development configuration is `12345678`, but deployments may use a different value. Clients must not hard-code the default password.

#### Login

##### `POST /api/v1/login`

Request:

```json
{"password":"12345678"}
```

A successful request returns `200 OK` and establishes a session through `Set-Cookie`:

```json
{"authenticated":true}
```

An incorrect password returns `401 Unauthorized` with the error code `invalid_password`.

#### Status and Device Identity

##### `GET /api/v1/status`

Returns the current network and debug-connection status:

```json
{
  "mode":"sta",
  "ssid":"Example-WiFi",
  "ip":"192.168.1.42",
  "ap_ssid":"AmphiLink-A1B2C3",
  "scanning":false,
  "dap_client":false
}
```

Possible `mode` values are `starting`, `connecting`, `sta`, and `ap`. `dap_client` indicates whether a TCP debug client is currently connected.

##### `GET /api/v1/device`

Returns the device identity:

```json
{"name":"AmphiLink","id":1}
```

##### `PUT /api/v1/device`

Updates and returns the device identity:

```json
{"name":"Bench-A","id":6}
```

`name` must contain 1-32 printable ASCII characters, and `id` must be an integer from `1` through `10`. After a successful update, the UDP broadcast payload and WS2812 indicator color are updated accordingly.

#### Saved Networks

The device stores up to 10 Wi-Fi profiles. Network-list responses use the following structure:

```json
{
  "revision":7,
  "locked":false,
  "items":[
    {
      "id":3,
      "ssid":"Example-WiFi",
      "has_password":true,
      "priority":0,
      "connected":true
    }
  ]
}
```

`priority` starts at `0`; a lower value indicates a higher automatic connection priority. `id` is a stable profile identifier and is not an array index. List and mutation responses never contain passwords; `has_password` only indicates whether a password is configured. When `locked` is true, the current admin password cannot decrypt the saved credentials, the revision is `0`, and the list is empty.

Every request that modifies the network list must include the most recently retrieved `revision`. A successful modification returns an incremented revision. If the revision does not match, the device returns `409 Conflict` with `revision_conflict`; the client should retrieve the list again and merge the user's changes. The current firmware also returns `revision_conflict` when adding a duplicate SSID or changing an existing profile to an SSID that is already saved.

An SSID must be a valid UTF-8 string from 1 through 32 bytes. An empty password denotes an open network; a nonempty password must be from 8 through 63 bytes.

| Method and path | Request body | Success status | Description |
| --- | --- | --- | --- |
| `GET /api/v1/networks` | None | `200 OK` | Retrieve the network list |
| `GET /api/v1/networks/{id}/password` | None | `200 OK` | Retrieve one network password |
| `POST /api/v1/networks` | `{"revision":7,"ssid":"...","password":"..."}` | `201 Created` | Add a network |
| `PUT /api/v1/networks/{id}` | `{"revision":7,"ssid":"...","password":"..."}` | `200 OK` | Update a network; omit the password to retain it |
| `DELETE /api/v1/networks/{id}` | `{"revision":7}` | `200 OK` | Delete the specified network |
| `PUT /api/v1/networks/order` | `{"revision":7,"ids":[3,1,2]}` | `200 OK` | Reorder all networks using an array of IDs |
| `POST /api/v1/networks/reset` | `{"confirm":true}` | `200 OK` | Erase all credentials and clear the locked state |

The single-password endpoint returns `{"password":"..."}`; an open network returns an empty string. This endpoint and the other network endpoints in the table require a valid login cookie and use `Cache-Control: no-store`. The reset endpoint is intended for the `locked` state and erases the original ciphertext and creates an empty `profiles_v2` store only when `confirm:true` is explicitly supplied.

The `ids` array in a reorder request must list every current network ID exactly once.

#### Wi-Fi Scan

Scanning uses an asynchronous operation model.

1. Call `POST /api/v1/scan`. On success, the service returns `202 Accepted`:

   ```json
   {"operation_id":12}
   ```

2. Poll `GET /api/v1/scan/12` until `state` is `succeeded` or `failed`:

   ```json
   {
     "operation_id":12,
     "state":"succeeded",
     "error":0,
     "items":[
       {
         "ssid":"Example-WiFi",
         "rssi":-48,
         "channel":6,
         "auth":"wpa2",
         "supported":true,
         "saved":true
       }
     ]
   }
   ```

Possible `state` values are `idle`, `running`, `succeeded`, and `failed`. Possible `auth` values are `open`, `wpa2`, `wpa3`, `wpa2_wpa3`, `legacy_wpa`, and `unsupported`. While a TCP debug client is active, the firmware rejects scan requests with `dap_client_active` to avoid interrupting the debug connection.

#### Switching Networks

Network switching also uses an asynchronous operation model.

1. Call `POST /api/v1/connect`:

   ```json
   {"ssid":"Example-WiFi","password":"example-password"}
   ```

   For a saved network, use `{"profile_id":3}` so the password never needs to be returned to the client. When the operation is accepted, the service returns `202 Accepted` with an `operation_id`. This request actively disconnects the current TCP debug client.

2. While the device remains reachable, poll `GET /api/v1/connect/{operation_id}`:

   ```json
   {
     "operation_id":13,
     "state":"succeeded",
     "error":0,
     "ssid":"Example-WiFi"
   }
   ```

A successfully connected profile is saved and promoted to the highest priority. Switching networks may change the device IP address. Clients should expect the current HTTP connection to be interrupted and rediscover the device through UDP broadcasts or DHCP information.

#### Error Responses

All API errors use the following structure:

```json
{"error":{"code":"invalid_profile"}}
```

Common error codes are listed below:

| HTTP status | `code` | Meaning |
| --- | --- | --- |
| `400 Bad Request` | `invalid_json` | JSON is missing, has an incorrect type, or contains invalid fields |
| `400 Bad Request` | `invalid_identity` | Device name or color identifier is invalid |
| `400 Bad Request` | `invalid_profile` | SSID or password is invalid |
| `400 Bad Request` | `invalid_size` | Field or body length is invalid |
| `400 Bad Request` | `invalid_id` | Operation ID in the path is invalid |
| `401 Unauthorized` | `invalid_password` | Login password is incorrect |
| `401 Unauthorized` | `unauthorized` | A valid session cookie was not provided |
| `423 Locked` | `credentials_locked` | The current admin password cannot decrypt the saved Wi-Fi credentials |
| `404 Not Found` | `not_found` | Configuration item, asynchronous operation, or resource does not exist |
| `409 Conflict` | `revision_conflict` | Network-list revision conflict |
| `409 Conflict` | `profile_limit` | The limit of 10 network profiles has been reached |
| `409 Conflict` | `dap_client_active` | An active debug connection prevents scanning |
| `409 Conflict` | `operation_busy` | Another Wi-Fi operation is in progress |
| `500 Internal Server Error` | `storage_error` | Persistent storage operation failed |
| `503 Service Unavailable` | `not_ready` | A required subsystem is not ready |
| `503 Service Unavailable` | `web_assets_missing` | Static configuration-page assets are unavailable |

The `error` field in an asynchronous result is the underlying ESP-IDF `esp_err_t` numeric value. `0` indicates success; nonzero values are provided for diagnostics only. Clients should use `state` and HTTP error codes as their primary status indicators and must not treat a specific numeric value as a stable public enumeration across firmware versions.
