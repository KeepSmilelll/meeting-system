# Development Progress Aligned To `D:\docs\development_order.md`

## Alignment Audit (2026-04-09)

- 结论：阶段框架仍沿用 `development_order.md`，但记录颗粒度没有完全对齐“纵向打通主链”。主要问题不是 Phase 名称错误，而是把韧性、观测、运维子项和主链混写。
- [批量] 旧版 `Current Snapshot` 与 `Phase 3.1 / 6.5 / 7.2` 大量重复；同一事实同时出现在“快照”和“阶段明细”，导致快照臃肿且失去阶段定位价值。
- [批量] `Phase 3.1` 已把会议主链、SFU 选路、故障回补、分布式锁、路由状态事件、管理接口、恢复指标一起塞进单个子阶段；后四类更像主链打通后的韧性/运维收口，不应继续堆成“当前快照”。
- [超额] `Phase 6.5` 已超出开发顺序里的“RTCP + NACK”基础面，实际已落地 `SR/LSR/DLSR RTT`、PLI cooldown、REMB 聚合/下发、推荐码率回馈，并有 SFU 测试覆盖。
- [超额] `Phase 7.2` 屏幕共享已超出“最小可用发收链路”，实际已有反馈消费、重传/关键帧计数、目标/已应用码率观测与自动化测试。
- [纰漏] `Phase 0.2 / 1.4 / 2.2` 若按设计文档交付物核对仍不能写高：`meeting-server/signaling/pkg/logger.go`、`meeting-server/signaling/pkg/snowflake.go`、`plasma-hawking/src/security/TokenManager.*`、`plasma-hawking/src/security/CryptoUtils.*` 仍为空壳；认证仍是简化签名 token + 内存样例用户，不是设计目标里的 Argon2id + JWT。
- [纰漏] 验证口径需要分层：`meeting-server/signaling` 的 `go test ./...` 与 `meeting-server/sfu/build_sfu_check` 的 `ctest` 已在 2026-04-09 实测通过；Qt 侧 `plasma-hawking/build_phase4_5_check` 的关键四测（`test_screen_share_session`、`test_meeting_controller_mute_all`、`test_meeting_runtime_smoke`、`test_meeting_client_process_smoke`）已在 2026-04-09 实测通过，但 `test_database` 仍存在构建目录不一致/执行证据不连续的问题。

## Current Snapshot

- 主链已打通到：登录 -> 建会/入会/离会 -> 会中状态同步 -> 单远端音频协商与 P2P 音频闭环 -> Go↔SFU 建房/发布者注册 -> 单路屏幕共享反馈闭环。
- 当前真正卡住阶段提升的不是“再补一个会议细节”，而是 3 个收口：多节点故障恢复长期稳定性、多人订阅模型与 tile 编排、音频/视频侧设备体验与自适应策略。
- `Phase 5` 本轮已推进：`ScreenShareSession` 已支持“屏幕共享优先、摄像头回退”的视频上行模式，并已从临时内嵌桥接收口为统一 `CameraCapture`；`MeetingController` + `VideoTile` 已完成“共享帧源/视频帧源”双路由，并新增 `activeVideoPeerUserId` 约束无共享场景只渲染当前视频目标 tile，且共享选择与视频目标选择已解耦；当当前视频目标关闭视频时会自动回落到可用远端，避免停留在无视频源目标。
- `Phase 5` 本轮已推进：`SignalingClient` 的 `MediaOffer/Answer` 已透传 `video_ssrc`，`MeetingController` 按 peer 维护远端 `video_ssrc` 并同步到 `ScreenShareSession`；接收侧已按 peer + SSRC 过滤 RTP/RTCP，切换视频目标后旧 peer/旧 SSRC 晚到包会被丢弃。
- `Phase 5.6` 本轮已推进：`AVSync` 新增视频-音频时钟偏差与延迟渲染建议计算；`MeetingController` 在远端视频入帧时已实现“严重失步丢帧 + 轻度超前延迟渲染”的组合策略，并接入按到期时间出队的小型渲染队列（含队列深度上限与状态切换失效机制），且对已到期帧按 PTS 执行稳定重排后渲染，并在出队阶段按音频时钟二次校准（超前帧短延迟回队列重试），同时在到期积压场景裁剪旧帧以控制渲染突发，并加入基于相邻视频 PTS 的连续渲染节拍约束（最小/最大 cadence）；渲染队列现已支持通过环境变量调整 queue depth / audio-driven delay / frames-per-drain / cadence，并为每帧施加音频对齐延后截止，避免首帧被无限回队列；`test_av_sync` 已补齐时钟换算/阈值/延迟建议断言。
- 差距仍在：当前是“超窗丢帧 + 有界延迟渲染 + 到期出队 + 到期帧PTS重排 + 音频时钟二次牵引 + 到期积压旧帧裁剪 + 连续渲染节拍约束”的轻量策略，尚未实现完整的音频主时钟调度器与可配置化 jitter buffer。
- 当前最可信的验证证据来自 Qt 关键链路测试：`test_screen_share_session`、`test_meeting_controller_mute_all`、`test_av_sync`、`test_meeting_runtime_smoke`、`test_meeting_client_process_smoke` 已在 2026-04-11 实测通过；Qt 全量测试口径仍碎片化，文档只保留已实测或已被源码/测试直接支撑的结论。

## Phase 0 - Shared Protocol Layer + Skeleton

### 0.1 Protobuf 协议定义

- 当前状态：`partial`
- 已完成：`MediaOffer/Answer` 已携带 `audio_ssrc/video_ssrc`，Go / Qt / SFU 生成物已同步。
- 差距：仍需按 `api_interfaces*.md` 审计消息全集、编号区间与兼容性；协议层不能只按当前主链使用面记为完成。

### 0.2 Go 信令服务器骨架

- 当前状态：`mostly_done`
- 已完成：`main.go`、配置加载、TCP 启动、Redis / MySQL / SFU 组件装配可运行，`go test ./...` 通过。
- 差距：`pkg/logger.go`、`pkg/snowflake.go` 仍为空壳，骨架交付物未完全收口。

### 0.3 Qt 客户端骨架

- 当前状态：`mostly_done`
- 已完成：Qt 项目、QML 路由、数据库/网络/控制层已足够支撑后续登录和会议主链。
- 差距：`common/Types.h`、`common/Logger.*`、`common/Config.*` 仍为空壳；按骨架交付物口径仍未 full-plan。

### 0.4 二进制帧编解码

- 当前状态：`done`

## Phase 1 - Go Signaling Core

### 1.1 TCP 网络层

- 当前状态：`done`

### 1.2 消息路由

- 当前状态：`done`

### 1.3 存储层

- 当前状态：`partial`
- 已完成：`MeetingLifecycleStore` 已接 `MeetingRepo + ParticipantRepo`；Redis 房间态、会话目录、房间总线、节点直投主路径已接通。
- 差距：`message_repo.go` 仍为空壳，存储层仍有增强功能所需仓储未补齐。

### 1.4 认证模块

- 当前状态：`partial`
- 已完成：登录、续期、登出、跨节点重复登录踢旧会话主路径可运行，`handler`/`auth` 测试通过。
- 差距：当前实现仍是简化签名 token 与内存用户认证，不是设计目标里的 Argon2id + JWT + 持久化用户路径。

## Phase 2 - Qt Client Login + Connection

### 2.1 SQLite 数据库初始化

- 当前状态：`done`
- 已完成：WAL / 外键 / busy_timeout、8 张业务表 + `message_fts`、索引与触发器已落地。
- 差距：`test_database` 当前只在部分构建目录存在，且 `build/Debug` 版本实测断言失败，验证证据需补齐。

### 2.2 安全与 Token 模块

- 当前状态：`partial`
- 已完成：会话缓存实际由 `UserManager + SettingsRepository` 支撑，自动恢复主路径可用。
- 差距：`TokenManager.*`、`CryptoUtils.*` 仍为空壳，这一子阶段不能按独立模块完成记账。

### 2.3 信令客户端网络层

- 当前状态：`mostly_done`
- 已完成：连接、重连、心跳、帧编解码、媒体 offer/answer 收发主路径已在运行时 smoke 中使用。
- 差距：TLS / 更完整恢复语义仍未收口。

### 2.4 应用层控制

- 当前状态：`partial`
- 已完成：`UserManager`、`AppStateMachine`、`MeetingController` 已支撑登录和会议主链。
- 差距：`EventBus.*` 仍为空壳，这一层是“主链可运行但规划交付未齐”。

### 2.5 登录页 UI

- 当前状态：`done`

## Phase 3 - Meeting Management

### 3.1 Go 信令侧

- 当前状态：`partial`
- 已完成：`Create/Join/Leave/Kick/MuteAll`、`room:{meeting_id}` / `room_members:*` / `sfu_route:{meeting_id}`、跨节点广播、TURN 临时凭证、按路由调用 `CreateRoom/AddPublisher/RemovePublisher/DestroyRoom` 主链可运行。
- [批量] 同一子阶段已经额外吞入节点池排序、联合轮询、失败节点隔离、回补建房、CAS 切路由、分布式恢复锁、路由状态去重、恢复指标、`/admin/sfu/nodes` 管理快照；这些是主链后的韧性/运维增强，不再放进快照区重复叙述。
- 差距：多节点高频抖动下的长期稳定性、恢复窗口参数、并发场景收益量化仍待收口。

### 3.2 Qt 客户端侧

- 当前状态：`mostly_done`
- 已完成：`MeetingController`、`ParticipantListModel`、建会/入会 UI、会中状态同步、主持人切换、活动共享者选择、通话记录写入主路径可运行。
- [超额] `MeetingController` 已提前引入按 peer 维度的 offer/answer 状态集合与清理逻辑（超出 1v1 最小闭环），为后续多人订阅预留。
- 已完成：`test_meeting_controller_mute_all` 已补断言，覆盖“`setActiveShareUserId` 仅接受 `sharing=true` 目标；无共享场景通过 `setActiveVideoPeerUserId` 选择视频目标且不污染共享焦点”语义。
- 差距：当前仍是“单音频目标 + 单活动共享者”模型，尚未达到完整多人订阅与宫格管理。

## Phase 4 - Audio Pipeline

### 4.1 音频采集

- 当前状态：`partial`
- 已完成：默认输入设备、`float mono/48kHz/20ms` 统一帧、合成音频测试模式已可运行。
- 差距：设备枚举/切换 UI 仍未完成。

### 4.2 Opus 编解码

- 当前状态：`partial`
- 已完成：主路径可运行，音频会话已支持最小码率调节。
- 差距：体验参数与观测仍待收口。

### 4.3 RTP 传输

- 当前状态：`partial`
- 已完成：最小 P2P 音频收发、`SR/RR/RTT` 主链成立，音频 RR 已驱动发送码率调整。
- 差距：更完整的反馈控制和长期稳定性仍待补齐。

### 4.4 音频播放

- 当前状态：`partial`
- 已完成：播放线程和基础链路已接入主流程。
- 差距：`AudioPlayer.cpp` 仍很薄，设备切换与运行态参数暴露不足。

### 4.5 集成联调

- 当前状态：`partial`
- 已完成：`Signaling -> MeetingController -> MediaSessionManager -> AudioCallSession -> RTP/UDP` 主链可运行；`test_meeting_controller_mute_all`、`test_meeting_runtime_smoke`、`test_meeting_client_process_smoke`、`audio_codec_smoke` 有实测支撑（2026-04-09）。
- 差距：Qt 测试证据仍碎片化，不能按“完整 phase 验证通过”记账。

## Phase 5 - Video Pipeline

### 5.1 摄像头采集

- 当前状态：`partial`
- 已完成：`CameraCapture` 已落地最小采集实现；`ScreenShareSession` 已新增 `setCameraSendingEnabled/cameraSendingEnabled`，发送线程支持“共享优先、摄像头回退”单路上行，并已回收到统一 `CameraCapture` 模块；`test_screen_share_session` 已覆盖 camera sending 可用/不可用分支，并在可发包条件下验证“摄像头 RTP 上行 -> 远端接收解码”闭环。
- 差距：仍缺“真实摄像头上行 RTP -> 远端渲染”的实机端到端验证，以及多远端帧源路由与 AVSync 证据。

### 5.2 H.264 硬件编码

- 当前状态：`partial`
- 已完成：屏幕共享路径已落地 `VideoEncoder` 和候选编码器探测链。
- 差距：摄像头主链未接通，且当前实现仍依赖 `sws_scale`，不能把它当成 `Phase 5` 完成证据。

### 5.3 RTP 视频封包

- 当前状态：`partial`
- 已完成：屏幕共享路径已具备 H.264 FU-A 分片与重组。
- 差距：能力目前主要服务于单路屏幕共享，不是摄像头视频通话主链。

### 5.4 H.264 硬件解码

- 当前状态：`partial`
- 已完成：`VideoDecoder` 已能解 H.264，并输出 `NV12` 或从 `YUV420P` 转成 `NV12` 供渲染层使用。
- 差距：仍不是完整硬解主链，也未与摄像头视频通话打通。

### 5.5 OpenGL NV12 渲染

- 当前状态：`partial`
- 已完成：`VideoRenderer`、`NV12Shader`、VAO 绑定、`GL_RED/GL_RG` 纹理上传已落地。
- 已完成：`VideoGrid.qml`、`VideoTile.qml`、`MeetingRoom.qml` 已补齐状态化承载与 1v1 宫格；`VideoTile` 已按 `selected&&sharing` / 非共享状态切换 `remoteScreenFrameSource` 与 `remoteVideoFrameSource`，并在无共享时仅对 `activeVideoPeerUserId` 对应 tile 显示实时帧；点击无共享视频 tile 将切换视频目标而不污染共享焦点语义，且切换时会清空旧的 `remoteVideoFrameStore`，避免遗留帧闪现。
- 已完成：`ScreenShareSession::recvLoop` 在已配置 peer 时仅接受该 peer 入站 RTP/RTCP，并在设置 `expectedRemoteVideoSsrc` 后仅接收匹配 SSRC；`MeetingController` 已消费 `MediaOffer/Answer(video_ssrc)` 并下发当前目标 SSRC。`test_screen_share_session` 已新增“错 SSRC 丢包、恢复后继续收包”断言并在关键四测通过。
- 已完成：`test_meeting_controller_mute_all` 已补 `activeVideoPeerUserId` 断言，覆盖“共享焦点跟随”“共享关闭后回落到当前视频目标”“所选远端关闭视频后自动回落到仍有视频的远端并清理旧帧”，并验证回落后旧目标 `MediaOffer` 被过滤、新目标协商可继续。
- 差距：当前仍是单远端视频目标语义，尚未进入多远端帧源编排与 AVSync 收口。

### 5.6 音视频同步 + 集成

- 当前状态：`partial`
- 已完成：`AVSync` 已补齐音频/视频时钟到毫秒换算、偏差阈值判定、偏差计算与延迟渲染建议；`MeetingController` 已在远端视频回调接入“超窗丢帧 + 轻度超前延迟渲染”策略，并引入按到期时间排序的小型渲染队列（8 深度上限、状态切换失效与清队）与到期帧 PTS 稳定重排，并在出队阶段按音频时钟对超前帧执行短延迟回队列重试，并在到期积压时裁剪旧帧避免突发渲染，并基于相邻视频 PTS 差值施加最小/最大连续渲染节拍约束，通过 render ticket 在切换视频目标/共享状态时抑制过期延迟帧；新增并扩展 `test_av_sync` 验证换算、判定与延迟建议规则。`test_meeting_runtime_smoke` 已补运行时视频证据口径（编码器可用时要求双方存在视频协商证据且 guest 侧观察到远端视频解码帧；若当前环境不具备 H.264 编码能力则降级为跳过严格视频断言）并改为动态端口以降低端口冲突抖动，同时在三条失败路径统一输出分阶段视频诊断摘要（offer/answer/degrade/decode）、`likely-module` 归因以及 `signaling-stage(login/create/join)` 阶段定位；`RuntimeSmokeDriver` 已把失败/等待/降级原因统一结构化为 `stage=...; reason=...`，`test_meeting_client_process_smoke` 会解析并输出 `host-stage/guest-stage` 归因，形成端到端阶段定位闭环，并对 tempdir/端口预留/服务端启动监听/客户端进程拉起等前置失败点补充 `stage/module` 归因提示。`RuntimeSmokeDriver` 已支持 `MEETING_SMOKE_REQUIRE_VIDEO` 门控，`test_meeting_client_process_smoke` 默认按本机编码能力自动决定是否对 guest 开启视频证据校验，仍可通过 `MEETING_PROCESS_SMOKE_REQUIRE_VIDEO=1/0` 强制覆盖，并在失败日志中输出证据门控、编码能力、结果状态/原因摘要与 `likely-module` 归因。针对“本机编码器不可用”场景，`MeetingController` 已在视频会话错误回调中执行“摄像头自动静音 + 重新协商”的降级收口，并将该场景从硬失败语义调整为可降级提示；`RuntimeSmokeDriver` 已补“编码器失败可降级白名单”（仅在未强制视频证据时生效），避免文案回退到 `failed` 语义时误触发 smoke 失败；同时在 `MEETING_SYNTHETIC_CAMERA=1` 场景下优先走 synthetic camera fallback，并与 `MEETING_SYNTHETIC_SCREEN` 解耦，规避 headless 摄像头线程断言且不影响真实摄像头路径；`test_meeting_runtime_smoke` 与 `test_meeting_client_process_smoke` 已在 2026-04-11 复测通过。两条 smoke 现分别支持 `MEETING_RUNTIME_SMOKE_SYNTHETIC_CAMERA=0` 与 `MEETING_PROCESS_SMOKE_SYNTHETIC_CAMERA=0` 切到真实摄像头路径，并统一通过 `MEETING_SMOKE_EXPECT_CAMERA_SOURCE`（兼容旧 `MEETING_SMOKE_EXPECT_REAL_CAMERA`）校验 `real-device/synthetic-fallback` 证据；本轮 synthetic 模式复测通过，real-camera 模式在当前无摄像头设备环境按 `SKIP(77)` 退出，避免误报失败。
- 差距：当前仍缺完整的音频主时钟调度器（含多策略切换与 jitter buffer 参数化），尚未达到完整 lip-sync 调度。

## Phase 6 - C++ SFU Core

### 6.1 UDP 网络层

- 当前状态：`partial`

### 6.2 信令 RPC 通道

- 当前状态：`done_for_current_scope`
- 已完成：Go -> SFU `CreateRoom/DestroyRoom/AddPublisher/RemovePublisher` 主链与 `QualityReport` 上报链路已接通。

### 6.3 房间管理

- 当前状态：`done_for_skeleton`

### 6.4 RTP 路由

- 当前状态：`mostly_done`
- 已完成：`AddPublisher -> MediaIngress -> UdpServer -> RtpRouter` 主链打通，SSRC 路由和重写能力已落地。
- 差距：更高并发和复杂拓扑验证仍待补齐。

### 6.5 RTCP + NACK

- 当前状态：`partial`
- [超额] 已完成：RR / SR 解析、`cumulative_lost` 修正、`SR/LSR/DLSR` RTT 回算、Generic NACK 重传、PLI 转发与 cooldown、REMB 聚合/转发、`SendEstimatedRembToPublisher` 与 BWE 建议码率反馈；`build_sfu_check` 的 `ctest` 已于 2026-04-09 实测通过。
- 差距：当前已超出“功能存在”，但还没到“多人会议复杂拓扑长期稳定”。

### 6.6 客户端改造 + 联调

- 当前状态：`partial`
- 已完成：客户端已能按 `sfu_address` 接入相关媒体链路，屏幕共享会话可消费 NACK / PLI / REMB。
- 差距：完整多人远端流模型与 UI 组织仍未落地。

## Phase 7 - Enhanced Features

- 聊天持久化 / FTS5：`partial`
  已完成：房间内聊天广播可运行；SQLite 侧 `message` / `message_fts` 表、触发器已建好。
  差距：`store/message_repo.go` 仍为空壳，服务端持久化主链未收口。
- 屏幕共享：`partial`
  [超额] 已完成：采集、编码、RTP 发送、FU-A 重组、解码、渲染帧存储、NACK/PLI/REMB 反馈消费、码率重配置观测与自动化测试。
  差距：多人订阅模型、共享者 UI 编排、长时稳定性仍未收口。
- NAT 穿透 / STUN / TURN：`stub_or_partial`
  差距：服务端已能下发 TURN 临时凭证，但 `StunClient.*`、`TurnClient.*` 仍为空壳。
- 动态 BWE：`partial`
  已完成：音频 RR 驱动码率调整，SFU 侧已可根据丢包/RTT/抖动反馈推荐码率。
  差距：视频侧降级策略和长期参数调优仍未完成。
- AEC / NS / AGC：`stub_or_partial`
- 文件传输 / 录制 / 滤镜：`pending`

## Current Critical Path

- 当前迭代方向（主链优先）：`Phase 5.2 + 5.3 + 5.6`，先把“摄像头上行编码/RTP -> 远端解码渲染 -> AVSync 证据”补齐为可验证闭环，再推进 `6.6` 多人订阅。
- 进入条件（已满足）：`build_phase4_5_check` 下 `test_meeting_controller_mute_all`、`test_meeting_runtime_smoke`、`test_meeting_client_process_smoke`、`test_screen_share_session` 已在 2026-04-09 实测通过，可作为视频主链扩展基线。
- 暂缓项（并行低优先）：`Phase 3.1` 长稳压测、Proto 全量兼容审计、`Phase 7` 增强特性收口。

## Agent Dispatch (Condensed)

- Proto Agent：收口 `signaling.proto / sfu_rpc.proto` 编号、消息全集与兼容性审计。
- Go-Sig Agent：继续做多节点恢复、路由一致性、持久化认证与聊天仓储收口。
- Qt-UI Agent：优先打通 `VideoGrid` / `VideoTile` 的远端视频承载与 1v1 编排。
- AV-Eng Agent：优先打通 `CameraCapture -> VideoEncoder -> RTP` 视频发送主链。
- Cpp-SFU Agent：优先保证 1v1 视频订阅转发主链稳定，再扩展复杂拓扑验证。
