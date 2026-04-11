# 各 Phase 任务划分与 Agent 分工实现功能模块

## 1. 文档目的

本文件用于把项目现有设计文档、Agent 分工约束、当前实现进度，整理成一份可直接用于拆任务、排优先级、下发给子代理执行的统一文档。

目标不是重复 `development_order.md`，而是回答 3 个执行问题：

1. 每个 Phase 纵向主链到底要打通到什么程度才算过关。
2. 每个 Phase 应该拆成哪些任务包，分别落到哪些功能模块。
3. 每个任务包应由哪个 Agent 主责，哪些 Agent 协作，哪些工作暂时不要提前展开。

## 2. 依据范围

本文件基于以下资料整理：

- 项目约束与目录边界：
  - `D:\meeting\AGENTS.md`
  - `D:\meeting\development_progress.md`
- 原始设计与开发顺序：
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\development_order.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\client_design.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\server_design.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\api_interfaces.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\api_interfaces_part2.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\docs\deployment_guide.md`
- 协议与当前骨架说明：
  - `D:\meeting\meeting-server\proto\PROTO_WORKFLOW.md`
  - `D:\meeting\meeting-server\signaling\README.md`
  - `D:\meeting\plasma-hawking\README.md`
- Agent 技能与职责边界：
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\project-overview\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\codex-agents\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\protobuf-protocol\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\go-signaling\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\qt-client\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\av-engine\SKILL.md`
  - `C:\Users\16706\.gemini\antigravity\playground\plasma-hawking\.agents\skills\cpp-sfu\SKILL.md`

关于“子代理讨论”：

- 当前仓库内未发现独立持久化的子代理讨论记录文件。
- 因此本文件采用两类可追溯输入替代：
  - `development_progress.md` 已确认的当前阶段结论。
  - 本线程已确认的推进方向结论：优先纵向打通主链，当前合理推进方向是 `Phase 5.1 + 5.5 + 6.6`，先完成 1v1 摄像头视频闭环，再扩展多人订阅与复杂稳定性。

## 3. 设计原则

### 3.1 Phase 划分原则

- 每个 Phase 只对应一个清晰的纵向里程碑，不把“主链可用”和“韧性/运维增强”混在同一出口条件里。
- 主链先过，再补横向能力；没有主链闭环的能力一律不提前记完成。
- 协议、目录、线程模型、测试口径必须在任务拆分时同时出现，不能只写功能名。

### 3.2 Agent 分工原则

- 每个任务包只能有一个主责 Agent。
- 协作 Agent 只负责输入适配、接口衔接、生成物同步，不抢主责模块。
- 协议变更必须由 Proto Agent 牵头，其他 Agent 不直接发散改消息定义。
- Qt-UI 负责状态、页面、模型和控制器；AV-Eng 负责媒体链路与设备能力；两者交界线在 `MeetingController` 与 `VideoTile/VideoGrid` 绑定层。
- Go-Sig 负责会话、鉴权、会议控制、跨节点路由、服务端持久化；Cpp-SFU 只负责媒体平面，不负责业务状态。

### 3.3 Phase 出口原则

- 出口必须是“可验证的主链结果”，不是“代码文件存在”。
- 测试、运行时 smoke、关键目录编译通过，至少命中一种。
- 若某 Phase 已有超额实现，不自动推高当前优先级；仍按主链缺口决定下一步。

## 4. Agent 总体职责矩阵

| Agent | 主责目录 | 主责模块 | 典型输出 | 不应主导的工作 |
|------|---------|---------|---------|--------------|
| Proto | `meeting-server/proto/*` + 各端生成代码 | `signaling.proto`、`sfu_rpc.proto`、信令类型、错误码、代码生成链 | 协议定义、编号审计、生成物同步 | 业务 Handler、QML 页面、媒体算法 |
| Go-Sig | `meeting-server/signaling/**` | TCP Session、Router、Auth、Meeting、Chat、File、Redis/MySQL、SFU RPC 客户端 | 可运行信令服务、Handler 测试、Redis/MySQL 主链 | 客户端 QML、FFmpeg/OpenGL、SFU RTP 转发 |
| Qt-UI | `plasma-hawking/src/app/**`、`src/storage/**`、`src/net/signaling/**`、`qml/**` | UserManager、MeetingController、ParticipantListModel、Dialog/Page/Panel、SQLite、本地状态机 | 可交互 UI、QML 组件、控制层绑定、SQLite 存储 | FFmpeg 编解码、RTP 算法、SFU 服务端实现 |
| AV-Eng | `plasma-hawking/src/av/**`、`src/net/media/**`、部分 `src/net/ice/**` | 采集、编解码、RTP/RTCP、JitterBuffer、Render、AVSync、BWE、设备能力 | 音视频主链、媒体测试、设备与编解码能力 | 会议业务流程、QML 布局逻辑、Redis/MySQL |
| Cpp-SFU | `meeting-server/sfu/**` | UDP Server、Room/Publisher/Subscriber、RTP Router、RTCP、NACK、BWE、Simulcast | 可运行 SFU、多人转发、SFU 测试 | 客户端控制器、业务鉴权、数据库设计 |

## 5. Phase 级任务划分

下文每个 Phase 都按 6 个维度描述：

- 纵向目标：这一阶段真正要打通的用户路径。
- 任务包：可直接派单的执行单元。
- 功能模块：本阶段涉及的代码模块。
- Agent 分工：主责与协作边界。
- 出口标准：进入下一阶段的最小条件。
- 当前状态：基于 `development_progress.md` 的当前判断。

### Phase 0：共享协议层 + 项目骨架

**纵向目标**

让 Go 信令、Qt 客户端、SFU 三端具备统一协议、统一目录与统一构建基础，做到“协议可生成、双端可编译、帧可互通”。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P0-A 协议定义与编号边界 | Proto | Go-Sig, Qt-UI, Cpp-SFU | `signaling.proto`、`sfu_rpc.proto`、`signal_type.go`、错误码 | 全量消息定义、编号范围一致、字段兼容 |
| P0-B Go 骨架 | Go-Sig | Proto | `main.go`、`config/*`、`pkg/*`、`server/*`、`protocol/*` | `go build` 可过的信令骨架 |
| P0-C Qt 骨架 | Qt-UI | AV-Eng, Proto | `CMakeLists.txt`、`src/common/*`、`Main.qml`、标题栏与主窗口 | Qt 工程可编译启动 |
| P0-D 二进制帧编解码 | Proto | Go-Sig, Qt-UI | `frame.go`、`SignalProtocol.*`、`test_protocol.cpp` | 9 字节帧头跨端一致 |

**功能模块范围**

- Proto：帧格式、信令枚举、错误码、生成脚本。
- Go-Sig：基础目录、配置、日志、ID、协议包。
- Qt-UI：基础窗口、基础目录、公共类型、日志。
- AV-Eng：`FFmpegUtils.h`、`RingBuffer.h` 这类底层通用构件。

**出口标准**

- `protoc` 生成 Go/C++ 代码无误。
- Go 端可编译。
- Qt 端可编译并起空壳窗口。
- 帧编解码测试通过。

**当前状态**

- 协议与帧主链已建立。
- `pkg/logger.go`、`pkg/snowflake.go`、`common/Logger.*`、`common/Config.*`、`common/Types.h` 仍不完整。
- 结论：`Proto` 主链可用，Go/Qt 骨架属于 `mostly_done` 而非完全收口。

### Phase 1：Go 信令服务器核心

**纵向目标**

完成“TCP 建连 -> 登录认证 -> 心跳续期 -> 断线清理 -> 单点登录踢旧会话”的服务端主链。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P1-A TCP 与 Session 模型 | Go-Sig | Proto | `server/tcp_server.go`、`session.go`、`session_manager.go` | 3 goroutine/连接模型稳定 |
| P1-B Router 与状态中间件 | Go-Sig | Proto | `handler/router.go`、`handler/handler.go` | 状态校验与分发建立 |
| P1-C Store 与模型 | Go-Sig | - | `store/*`、`model/*` | Redis/MySQL 接入与模型定义 |
| P1-D Auth 主链 | Go-Sig | Qt-UI | `auth/*`、`handler/auth_handler.go` | 登录、登出、心跳、踢旧会话 |

**功能模块范围**

- 会话生命周期、读写背压、心跳超时、安全关闭。
- Redis `session:{user_id}` 与限流计数。
- JWT 与 Argon2id。
- GORM 用户表与认证相关仓储。

**出口标准**

- 登录成功返回 token。
- 重复登录踢旧会话。
- 心跳续期和超时断线可验证。
- `go test ./...` 通过。

**当前状态**

- Session/Router/基础鉴权主链已可运行。
- 当前认证实现仍是简化 token + 样例用户路径，不是设计目标里的 Argon2id + JWT 完整版。
- 结论：Phase 1 主链可用，但安全交付未完全达标。

### Phase 2：Qt 客户端登录 + 连接

**纵向目标**

完成“客户端启动 -> 连接信令 -> 登录/自动登录 -> 首页可用 -> 网络断线可恢复”的客户端接入主链。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P2-A SQLite 初始化 | Qt-UI | - | `DatabaseManager`、`SettingsRepository`、`UserRepository` | 8 表 + FTS5 + 独立 DB 线程 |
| P2-B Token 与安全模块 | Qt-UI | Go-Sig | `TokenManager.*`、`CryptoUtils.*` | Token 缓存与登录前摘要 |
| P2-C SignalingClient 主链 | Qt-UI | Proto | `SignalingClient.*`、`Reconnector.*`、`SignalProtocol.*` | 帧发送/接收、心跳、重连 |
| P2-D 应用层状态机 | Qt-UI | - | `UserManager.*`、`AppStateMachine.*`、`EventBus.*` | 登录态、连接态、自动恢复 |
| P2-E 登录 UI | Qt-UI | - | `LoginDialog.qml`、`HomePage.qml`、`TitleBar.qml`、`Main.qml` | 登录页与首页切换 |

**功能模块范围**

- 本地数据库、设置缓存、用户缓存。
- Token 持久化与过期判断。
- 信令连接、断线重连、粘包处理。
- C++ 到 QML 的状态暴露。

**出口标准**

- SQLite 自动建表。
- 登录成功后跳转首页。
- 断网后可重连。
- Token 恢复可用。

**当前状态**

- SQLite、SignalingClient、MeetingController 基础链路已支撑后续会议主链。
- `TokenManager.*`、`CryptoUtils.*`、`EventBus.*` 仍为空壳或未完成。
- 结论：Phase 2 是“主链已成、规划模块未齐”。

### Phase 3：会议管理垂直打通

**纵向目标**

完成“登录 -> 创建会议 -> 加入会议 -> 参会者同步 -> 离会/主持人转移”的业务主链。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P3-A 会议仓储与房间状态 | Go-Sig | - | `meeting_repo.go`、`participant_repo.go`、Redis `room:*` | 房间状态与成员状态稳定 |
| P3-B 会议 Handler 主链 | Go-Sig | Cpp-SFU | `meeting_handler.go`、`media_handler.go`、TURN 凭证、SFU 路由 | Create/Join/Leave/Kick/MuteAll 全流程 |
| P3-C 会中控制器与模型 | Qt-UI | Go-Sig | `MeetingController.*`、`ParticipantListModel.*` | 会中状态与参会者同步 |
| P3-D 会议 UI 与本地记录 | Qt-UI | - | `CreateMeetingDialog.qml`、`JoinMeetingDialog.qml`、`MeetingRoom.qml`、`CallLogRepository.*` | 房间页、参与者面板、通话记录 |

**功能模块范围**

- Go 侧：Meeting Lifecycle、跨节点广播、SFU 地址下发、TURN 凭证下发。
- Qt 侧：MeetingController 属性、ParticipantListModel、Room 页面、侧栏。

**出口标准**

- 双端可完成建会/入会/离会。
- 参会者列表实时同步。
- 主持人转移生效。
- 空房清理与房间状态回收成立。

**当前状态**

- 主链已明显超出最初“最小会议管理”范围。
- 但 `Phase 3.1` 已混入故障恢复、管理接口、去重、恢复锁等韧性能力。
- 结论：会议主链已通，后续属于稳定性与多节点治理，不应继续占用“当前主链快照”。

### Phase 4：音频管线端到端

**纵向目标**

完成“会中媒体协商 -> 音频采集 -> Opus 编码 -> RTP/RTCP -> 解码播放 -> 设备切换”的 1v1 P2P 音频闭环。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P4-A 采集与设备管理 | AV-Eng | Qt-UI | `AudioCapture.*`、设备枚举/切换 | 输入设备主链 |
| P4-B Opus 编解码 | AV-Eng | - | `AudioEncoder.*`、`AudioDecoder.*` | 20ms 帧编解码稳定 |
| P4-C RTP/RTCP/Jitter | AV-Eng | Proto | `RTPSender.*`、`RTPReceiver.*`、`JitterBuffer.*`、`RTCPHandler.*` | P2P 音频传输与反馈 |
| P4-D 播放与设置页 | AV-Eng | Qt-UI | `AudioPlayer.*`、`AudioSettings.qml` | 输出设备与音量控制 |
| P4-E 集成烟测 | AV-Eng | Qt-UI, Go-Sig | smoke tests、MeetingController 媒体接线 | 可重复验证的 1v1 音频闭环 |

**功能模块范围**

- 音频采集线程、编码线程、播放线程。
- RTP/RTCP 基础控制。
- 最小 RR/RTT 与发送码率调整。
- 设置页与设备切换入口。

**出口标准**

- 两端互相听到声音。
- RR/RTT 可计算。
- 设备切换可生效。
- 长时运行不泄漏。

**当前状态**

- 1v1 音频主链已具备可信验证。
- `test_meeting_controller_mute_all`、`test_meeting_runtime_smoke`、`test_meeting_client_process_smoke` 已通过。
- 设备切换、长稳、体验侧参数仍未收口。
- 结论：Phase 4 主链已过，剩余问题属于体验与完整验证。

### Phase 5：视频管线端到端

**纵向目标**

在已有音频基础上，完成“摄像头采集 -> H.264 编码 -> RTP 视频发送 -> 解码 -> NV12 渲染 -> VideoTile/Grid 展示 -> 音视频同步”的 1v1 视频闭环。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P5-A 摄像头采集 | AV-Eng | Qt-UI | `CameraCapture.*`、设备枚举与切换 | 摄像头帧稳定进入编码链 |
| P5-B 视频编码 | AV-Eng | - | `VideoEncoder.*`、动态码率、关键帧请求 | H.264 编码主链 |
| P5-C 视频 RTP | AV-Eng | Proto | `RTPSender.*`、`RTPReceiver.*`、FU-A 分片/重组 | 视频 RTP 主链 |
| P5-D 解码与渲染 | AV-Eng | Qt-UI | `VideoDecoder.*`、`VideoRenderer.*`、`NV12Shader.*`、shader 文件 | 远端画面真实可渲染 |
| P5-E UI 接线与 AVSync | Qt-UI | AV-Eng | `VideoTile.qml`、`VideoGrid.qml`、`MeetingRoom.qml`、`AVSync.*` | 1v1 视频房间可见可控 |

**功能模块范围**

- 采集：摄像头、多设备切换、帧队列。
- 编码：硬编优先、关键帧请求、动态码率。
- 传输：视频 SSRC、90kHz 时间戳、FU-A 分片。
- 渲染：NV12 双纹理、VAO、Shader、Y 轴翻转。
- UI：VideoTile、VideoGrid、用户名/静音/占位态。

**出口标准**

- 两端能看到对方视频并保持音频同时工作。
- 硬件编解码实际生效。
- AVSync 在可接受范围内。
- 宫格与 Tile 至少在 1v1 下稳定显示。

**当前状态**

- 屏幕共享路径已带动一部分编码、封包、解码、渲染能力提前实现。
- 但 `CameraCapture`、`VideoGrid`、`VideoTile` 仍未真正承接摄像头视频主链，`AVSync` 也未完成。
- 结论：Phase 5 仍是当前最合理的主攻方向。

### Phase 6：C++ SFU 媒体服务器

**纵向目标**

完成“Go 选路建房 -> 客户端接入 SFU -> 多远端流转发 -> RTCP/NACK/PLI/REMB -> 3 人以上多人会议”的媒体平面主链。

| 任务包 | 主责 Agent | 协作 Agent | 功能模块 | 交付结果 |
|------|-----------|-----------|---------|---------|
| P6-A UDP 与 RPC 基础层 | Cpp-SFU | Go-Sig, Proto | `UdpServer.*`、RPC Listener、RPC 帧协议 | SFU 基础控制面可用 |
| P6-B 房间与发布者模型 | Cpp-SFU | Go-Sig | `RoomManager.*`、`Room.*`、`Publisher.*`、`Subscriber.*` | 房间与发布者状态可管理 |
| P6-C RTP 路由 | Cpp-SFU | - | `RtpParser.*`、`RtpRouter.*`、SSRC 重写 | 纯转发主链 |
| P6-D RTCP 与重传 | Cpp-SFU | AV-Eng | `RtcpHandler.*`、`NackBuffer.*`、`BandwidthEstimator.*` | NACK/PLI/REMB/BWE 反馈链 |
| P6-E 客户端多流接入 | AV-Eng | Qt-UI, Go-Sig, Cpp-SFU | `MediaSessionManager`、多解码器、`VideoGrid.qml` | 3+ 人会议最小可用 |

**功能模块范围**

- SFU 控制面：CreateRoom/DestroyRoom/AddPublisher/RemovePublisher。
- 媒体面：Publisher/Subscriber、RTP/RTCP、带宽估计。
- 客户端改造：JoinRsp.sfu_address 消费、多远端流绑定、宫格升级。

**出口标准**

- 3 人会议每人看到其余两人。
- NACK/PLI/REMB 起效。
- Go 信令与 SFU RPC 主链稳定。
- 客户端多流接收与 UI 承载可运行。

**当前状态**

- Go 与 SFU 的建房/发布者注册主链已通。
- RTCP + NACK + RTT + PLI cooldown + REMB 已明显超出基础面。
- 真正未完成的是 `6.6` 客户端多远端流组织和多人 UI 承载。
- 结论：Phase 6 服务端能力领先，客户端多流消费落后于 SFU 侧。

### Phase 7：增强功能

**纵向目标**

在 Phase 0~6 主链成立后，以“独立功能包”的形式逐个交付，不反向拖垮主链优先级。

| 子阶段 | 主责 Agent | 协作 Agent | 功能模块 | 当前判断 |
|------|-----------|-----------|---------|---------|
| P7.1 聊天 + 持久化 | Go-Sig | Qt-UI | `chat_handler.go`、`message_repo.go`、`ChatPanel.qml`、`MessageRepository.*` | UI 与 SQLite 预埋较多，服务端持久化未完全收口 |
| P7.2 屏幕共享 | AV-Eng | Qt-UI, Go-Sig, Cpp-SFU | `ScreenCapture.*`、共享状态广播、共享 Tile/大画面 | 已超出最初最小范围，属于提前完成较多 |
| P7.3 NAT 穿透 | AV-Eng | Go-Sig | `StunClient.*`、`TurnClient.*`、ICE 候选链 | 仍明显不足，当前多为空壳 |
| P7.4 动态 BWE | AV-Eng | Cpp-SFU | `BandwidthEstimator.*`、编码器重配置 | 音频与 SFU 侧已有基础，视频闭环未收口 |
| P7.5 AEC/NS/AGC | AV-Eng | - | `process/*` | 仍未成链 |
| P7.6 文件传输 | Go-Sig | Qt-UI | `file_handler.go`、`FileTransfer.*`、面板与仓储 | 尚未进入主链 |
| P7.7 录制 | AV-Eng | Qt-UI | `MeetingRecorder.*`、录制管理 | 低优先 |
| P7.8 滤镜插件 | AV-Eng | Qt-UI | `filter/*`、插件管理 | 低优先 |

**出口标准**

- 每个增强功能独立建账、独立验证，不再和主链 Phase 混写。

**当前状态**

- 屏幕共享和 SFU RTCP/BWE 能力已经有超额实现。
- NAT/TURN、音频前处理、文件传输等仍应保持低优先级，避免偏离当前主链。

## 6. 当前推荐推进顺序

基于当前实现状态，合理推进顺序应为：

1. `Phase 5.1 + 5.5`
   - 优先补齐 `CameraCapture`、`VideoTile`、`VideoGrid`、`MeetingRoom` 的真实视频承载。
   - 原因：这是当前最大的主链空洞，且已经有音频主链、编码/渲染基础和 smoke 基线可复用。

2. `Phase 5.2 + 5.3 + 5.4`
   - 在 UI 承载路径明确后，把摄像头视频编码、RTP 视频发送、解码与渲染闭环打满。
   - 原因：避免先堆协议与算法，再发现 UI 与控制层接不住。

3. `Phase 6.6`
   - 把当前领先的 SFU 服务端能力真正接到客户端多远端流模型上。
   - 原因：没有 1v1 视频闭环作为地基，多人订阅会把问题放大而不是解决。

4. 低优先并行收口
   - `Phase 3.1` 多节点高频故障恢复长稳验证。
   - Proto 全量兼容性审计。
   - `Phase 7.3/7.5/7.6` 等增强功能。

## 7. 推荐派单方式

建议以后每次只下发“一个 Phase 下的一个任务包”，不要直接下发整段 Phase。推荐模板如下：

```markdown
## Task: [任务包标题]

**Phase**: [例如 Phase 5 / P5-A]
**Lead Agent**: [Proto / Go-Sig / Qt-UI / AV-Eng / Cpp-SFU]
**Support Agent**: [可为空，或 1~2 个]

### Vertical Goal
[本任务要打通的用户链路，不超过两句]

### Modules
- [模块 1]
- [模块 2]

### Files
- [明确文件路径]

### Dependencies
- [前置条件]

### Acceptance
- [可执行验证项]
- [Smoke / 单测 / 编译要求]
```

## 8. 可直接执行的近期任务包

### T1：1v1 视频承载底座

- Phase：`P5-A + P5-E`
- Lead Agent：`Qt-UI`
- Support Agent：`AV-Eng`
- 模块：
  - `qml/components/VideoTile.qml`
  - `qml/components/VideoGrid.qml`
  - `qml/pages/MeetingRoom.qml`
  - `src/app/MeetingController.*`
- 目标：
  - 先让会中页面能稳定承接“本地/远端视频源”的显示状态、布局与绑定。

### T2：摄像头采集与视频上行主链

- Phase：`P5-A + P5-B + P5-C`
- Lead Agent：`AV-Eng`
- Support Agent：`Qt-UI`
- 模块：
  - `src/av/capture/CameraCapture.*`
  - `src/av/codec/VideoEncoder.*`
  - `src/net/media/RTPSender.*`
  - `src/net/media/RTPReceiver.*`
- 目标：
  - 把本地摄像头帧送入 H.264 -> RTP 视频链，形成最小视频上行。

### T3：远端视频解码与渲染闭环

- Phase：`P5-D`
- Lead Agent：`AV-Eng`
- Support Agent：`Qt-UI`
- 模块：
  - `src/av/codec/VideoDecoder.*`
  - `src/av/render/VideoRenderer.*`
  - `src/av/render/NV12Shader.*`
  - `shaders/*`
- 目标：
  - 让远端 H.264 RTP 流能真实渲染到 `VideoTile`。

### T4：多人订阅升级

- Phase：`P6-E`
- Lead Agent：`AV-Eng`
- Support Agent：`Qt-UI`, `Cpp-SFU`, `Go-Sig`
- 模块：
  - `src/net/media/*`
  - `src/app/MeetingController.*`
  - `qml/components/VideoGrid.qml`
  - SFU `room/*`、`rtp/*`
- 目标：
  - 在 1v1 视频稳定后，升级到多远端流解码与宫格承载。

## 9. 结论

从项目现状看，当前不是“缺更多功能点”，而是“Phase 5 的摄像头视频主链还没有真正纵向打通”。因此：

- 会议主链、音频主链、Go↔SFU 控制主链已经足够作为基线。
- 真正应该优先拆单的是视频主链，不是继续扩展聊天、文件、NAT、录制等增强项。
- 后续所有 Agent 分工，都应围绕“先把 1v1 视频闭环做实，再把多人订阅做稳”来安排。
