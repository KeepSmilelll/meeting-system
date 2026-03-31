# Development Progress Aligned To `D:\docs\development_order.md`

## Current Snapshot

- 当前主线能力已经稳定打通到 `Phase 3`，并且关键基础设施比派工前前进了一步：
  - Go 会议主流程已切到 `MeetingLifecycleStore` 适配层，可在 MySQL 可用时走 `MeetingRepo` 主路径，并保留 `MemoryStore` 回退。
  - Qt `DatabaseManager` / `AppStateMachine` 已实现，数据库入口和状态机主路径不再是 `TODO`。
  - 音频侧已补上真实默认设备 `AudioCapture` / `AudioPlayer` 接入，RTCP SR/RR 最小闭环仍保留。
  - SFU `AddPublisher -> MediaIngress -> UdpServer -> RtpRouter` 主链路已打通，且已修回与 Go 侧兼容的 protobuf wire 包体。
- 当前最关键的未闭环项变成：
  - Qt protobuf 运行时环境阻塞已解除，`meeting_client` 现在可以完整构建；但客户端运行态联调仍未完成。
  - 音频链路已补上 `MediaSessionManager + AudioCallSession + P2P MediaOffer/Answer + Qt Multimedia` 运行态接线，并通过本地 smoke / 构建验证；当前剩余的是双实例 UI 烟测。
  - Go 会议持久化主路径已前进一步，但 `ParticipantRepo` 还没有完全进入 handler 主链路。
  - SFU 已可处理真实 UDP 入口，但 Go↔SFU 的端到端联调尚未在当前回合完成。

## Phase 0 - Shared Protocol Layer + Skeleton

### 0.1 Protobuf 协议定义

- `signaling.proto` 已存在。
- `sfu_rpc.proto` 已存在。
- `generate.ps1` / `generate.sh` 已存在。
- Go / Qt / SFU 生成代码已存在。
- 当前状态：`partial`
- 差距：
  - 需要按 `api_interfaces.md` / `api_interfaces_part2.md` 再做一次消息全集、编号范围、向后兼容性审计。
  - 当前工作区有 proto 与生成文件改动，需由 Proto Agent 统一收口。

### 0.2 Go 信令服务器骨架

- `go.mod`、`main.go`、`config`、`pkg/logger.go`、`pkg/snowflake.go` 已存在。
- `go build` 主路径已具备基础条件。
- 当前状态：`done`

### 0.3 Qt 客户端骨架

- `CMakeLists.txt`、`Main.qml`、`TitleBar.qml`、公共目录结构已存在。
- `src/common/FFmpegUtils.h`、`RingBuffer.h`、`Types.h`、`Logger.h/cpp` 已存在。
- 当前状态：`mostly_done`
- 差距：
  - 需要结合实际构建结果确认这些公共模块是否全部达到 full-plan 水平，而不只是占位存在。

### 0.4 二进制帧编解码

- Go `protocol/frame.go` / `signal_type.go` 已存在。
- Qt `SignalProtocol.h/cpp` 已存在。
- `frame_test.go`、`test_protocol.cpp` 已存在。
- 当前状态：`done`

## Phase 1 - Go Signaling Core

### 1.1 TCP 网络层

- `tcp_server.go`、`session.go`、`session_manager.go` 已存在。
- 认证超时、心跳相关主流程已具备。
- 当前状态：`done`

### 1.2 消息路由

- `handler/router.go` 已存在。
- 读帧后分发到 handler 的主路径已具备。
- 当前状态：`done`

### 1.3 存储层

- `mysql.go`、`redis.go`、`user_repo.go`、`meeting_repo.go`、`participant_repo.go` 已存在。
- `model/user.go`、`meeting.go`、`participant.go`、`message.go`、`friendship.go` 已存在。
- 当前状态：`partial`
- 差距：
  - `MeetingHandler` 仍依赖 `MemoryStore`，数据库仓储不是主数据路径。
  - `ParticipantRepo` 当前仍是内存仓储风格，并非最终 GORM 持久化实现。
  - 跨节点广播所需的 Redis Pub/Sub 房间主路径尚未闭环。

### 1.4 认证模块

- `auth/jwt.go`、`auth/rate_limiter.go`、`handler/auth_handler.go` 已存在。
- Login / Logout / Heartbeat 主流程已存在。
- 当前状态：`partial`
- 差距：
  - 需要复核是否完全符合 full-plan 的 Argon2id、Redis 会话、限流、断线清理口径。
  - 需要确认测试覆盖是否覆盖 production path，而不只是内存路径。

## Phase 2 - Qt Client Login + Connection

### 2.1 SQLite 数据库初始化

- `DatabaseManager` 已实现 WAL、`foreign_keys`、`busy_timeout`、统一建表、索引与 FTS 触发器。
- `SettingsRepository`、`UserRepository`、`MessageRepository`、`MeetingRepository`、`CallLogRepository` 已接入统一数据库入口。
- 当前状态：`done`

### 2.2 安全与 Token 模块

- `TokenManager`、`CryptoUtils` 已存在。
- 当前状态：`mostly_done`
- 差距：
  - 需要与新的数据库初始化、自动登录路径联调确认。

### 2.3 信令客户端网络层

- `SignalingClient`、`Reconnector` 已存在。
- 登录、心跳、创建会议、加入会议、离会等 protobuf 流已具备。
- Qt 客户端 protobuf 生成物已重新对齐到 `protoc 3.19.1 + --cpp_out=lite:`，`meeting_client` 已恢复可构建。
- 当前状态：`mostly_done`
- 差距：
  - TLS 握手仍未对齐 full-plan。
  - 需与 AppStateMachine/持久化方案重新对齐。

### 2.4 应用层控制

- `AppStateMachine` 已实现并挂入 QML 上下文。
- `Main.qml` 已按 `Disconnected -> Connected -> LoggedIn -> InMeeting` 主线驱动页面切换。
- 当前状态：`mostly_done`
- 差距：
  - `EventBus` 仍未实现，但当前主路径不依赖它。

### 2.5 登录页 UI

- `LoginDialog.qml`、`HomePage.qml`、`Main.qml` 已存在。
- 当前状态：`done`

## Phase 3 - Meeting Management

### 3.1 Go 信令侧

- Create / Join / Leave / Kick / MuteAll 主流程已存在。
- 主持人转移、空房清理、MeetingMirror 注入缝已存在。
- 当前状态：`partial`
- 差距：
  - `MeetingRepo` / `ParticipantRepo` 尚未接成主路径。
  - TURN 临时凭证未实现。
  - SFU 节点选择、跨节点广播仍未完整对齐。

### 3.2 Qt 客户端侧

- `MeetingController`、`ParticipantListModel`、`CreateMeetingDialog`、`ParticipantPanel`、`MeetingRoom.qml` 已存在。
- 本地 meeting history / call log 已接入。
- 当前状态：`mostly_done`
- 差距：
  - 需要和数据库主路径、多人媒体状态绑定进一步对齐。

## Phase 4 - Audio Pipeline

### 4.1 音频采集

- `AudioCapture` 文件已存在。
- 当前状态：`partial`
- 已完成：
  - `AudioCapture` 已接入 Qt Multimedia 默认输入设备，内部统一转成 `float` mono / 48kHz / 20ms 帧，并保留 `pushCapturedFrame` 注入路径。
- 差距：
  - 设备枚举/切换能力仍未做成可配置 UI；当前只覆盖默认输入设备。

### 4.2 Opus 编解码

- `AudioEncoder` / `AudioDecoder` 已存在。
- `audio_codec_smoke` 已存在，并已恢复通过。
- 当前状态：`partial`
- 差距：
  - 当前以 FFmpeg 主路径 + PCM fallback 先保证 Phase 4.5 纵向闭环，后续仍需把纯 Opus 主路径稳定化。

### 4.3 RTP 传输

- `RTPSender`、`RTPReceiver`、`JitterBuffer`、`RTCPHandler` 文件已存在。
- 当前状态：`partial`
- 差距：
  - RTCP 仍只有轻量解析，不是完整 SR/RR/NACK/PLI 主流程。
  - 已通过本地 UDP/RTP/抖动缓冲 smoke，真实双端联调仍待补。

### 4.4 音频播放

- `AudioPlayer` 已实现为线程化 PCM 播放核心，具备队列、增益、统计与 `AVSync` 音频主时钟更新。
- 当前状态：`partial`
- 已完成：
  - `AudioPlayer` 已接入 Qt Multimedia 默认输出设备；无 `QCoreApplication` 或设备格式不稳时会回退到原有内存播放主路径。
- 差距：
  - 仍缺少设备枚举/切换与运行态音频参数暴露。

### 4.5 集成联调

- 当前状态：`partial`
- 已完成：`SignalingClient(MediaOffer/Answer) -> MediaSessionManager -> AudioCallSession -> AudioCapture -> RTPSender -> UDP -> RTPReceiver -> JitterBuffer -> AudioDecoder -> AudioPlayer` 主链路接线；`meeting_client`、`audio_codec_smoke`、`test_media_session_manager`、`test_database` 与 Qt 侧 `ctest` 已通过。
- 差距：
  - 仍需完成双实例 UI 烟测，确认真实麦克风/扬声器路径在运行态双端可用。
## Phase 5 - Video Pipeline

### 5.1 摄像头采集

- `CameraCapture` 文件已存在。
- 当前状态：`stub_or_partial`

### 5.2 H.264 硬件编码

- `VideoEncoder` 文件已存在。
- 当前状态：`stub_or_partial`

### 5.3 RTP 视频封包

- 当前状态：`pending`

### 5.4 H.264 硬件解码

- `VideoDecoder` 文件已存在。
- 当前状态：`stub_or_partial`

### 5.5 OpenGL NV12 渲染

- `VideoRenderer`、`NV12Shader`、shader 文件已存在。
- 当前状态：`stub_or_partial`

### 5.6 音视频同步 + 集成

- `AVSync`、`VideoTile.qml`、`VideoGrid.qml` 文件已存在。
- 当前状态：`stub_or_partial`

## Phase 6 - C++ SFU Core

### 6.1 UDP 网络层

- `UdpServer`、`DtlsContext` 文件已存在。
- 当前状态：`partial`
- 差距：
  - 需要从“可收包骨架”推进到“可驱动房间/RTP 路由”的主路径。

### 6.2 信令 RPC 通道

- `RpcProtocol`、`RpcServer`、`RpcService` 已存在。
- `sfu_rpc_server_tests.cpp` 已覆盖 frame round-trip、服务生命周期、loopback 与媒体入口。
- 当前状态：`done_for_current_scope`

### 6.3 房间管理

- `RoomManager`、`Room`、`Publisher`、`Subscriber` 已存在。
- 当前状态：`done_for_skeleton`

### 6.4 RTP 路由

- `MediaIngress` 已把真实 UDP 收包、房间查找、Publisher 解析、`RtpRouter` 和 Subscriber 回调接起来。
- 当前状态：`mostly_done`
- 差距：
  - 仍需补齐更完整的 RTCP/NACK/PLI/REMB 运行态联调。

### 6.5 RTCP + NACK

- `RtcpHandler`、`NackBuffer`、`BandwidthEstimator` 已存在。
- 当前状态：`partial`
- 差距：
  - 仍需进入真实 SFU 转发/反馈主流程。

### 6.6 客户端改造 + 联调

- 当前状态：`pending`

## Phase 7 - Enhanced Features

- 聊天持久化 / FTS5：`partial`
- 屏幕共享：`stub_or_partial`
- NAT 穿透 / STUN / TURN：`stub_or_partial`
- 动态 BWE：`stub_or_partial`
- AEC / NS / AGC：`stub_or_partial`
- 文件传输 / 录制 / 滤镜：`pending`

## Current Critical Path

1. 完成双实例 P2P 语音 UI 烟测，确认真实麦克风/扬声器路径在运行态双端可用。
2. 把 Go 会议持久化继续推进到更完整的 repo 主路径，尤其是 ParticipantRepo 与更多生产数据源。
3. 做一次 Go↔SFU 真实 RPC 联调，确认修复后的 protobuf wire 兼容层在跨进程路径上成立。
4. 补一次 Qt 运行态登录/建会/入会最小烟测，并把音频联调并入这条端到端路径。

## Agent Dispatch

### Proto Agent

- 负责范围：
  - `meeting-server/proto/*`
  - `plasma-hawking/proto/*`
  - 所有受 proto 影响的生成代码
- 当前任务：
  - 审计 `signaling.proto` / `sfu_rpc.proto` 是否与 `development_order.md`、`api_interfaces*.md` 对齐。
  - 核对消息全集、类型编号、向后兼容性。
  - 若有缺口，统一修改 proto 与生成代码，不在业务代码中临时兜底。

### Go-Sig Agent

- 负责范围：
  - `meeting-server/signaling/**`
- 当前任务：
  - 以 `Phase 3.1` 为主，完成 `MeetingRepo` / `ParticipantRepo` 接入设计与最小实现。
  - 解决字符串 `meeting_id` / `user_id` 与持久化层 `uint64` 的映射问题。
  - 保持现有内存路径可回退，不破坏当前测试。

### Qt-UI Agent

- 负责范围：
  - `plasma-hawking/src/app/**`
  - `plasma-hawking/src/storage/**`
  - `plasma-hawking/qml/**`
- 当前任务：
  - 以 `Phase 2.1` / `2.4` 为主，完成 `DatabaseManager`、统一建表、`AppStateMachine` 主路径接入。
  - 让登录状态、重连状态、首页/会议页切换基于明确状态机，而非散落状态。

### AV-Eng Agent

- 负责范围：
  - `plasma-hawking/src/av/**`
  - `plasma-hawking/src/net/media/**`
- 当前任务：
  - 以 `Phase 4` 为主，优先补齐 `AudioPlayer`、RTCP 主流程缺口，并审计当前采集/编解码/RTP/Jitter 实现是否满足 full-plan。
  - 目标是形成“离双端语音闭环还差什么”的精确补丁，而不是泛泛扫尾。

### Cpp-SFU Agent

- 负责范围：
  - `meeting-server/sfu/**`
- 当前任务：
  - 以 `Phase 6.1` ~ `6.5` 为主，推进 `UdpServer` + `RpcService` + `RoomManager` + `RtpRouter` 的真实主链路设计。
  - 重点补 `AddPublisher` 后的 UDP 媒体入口信息、房间查找、RTP 路由接线。



