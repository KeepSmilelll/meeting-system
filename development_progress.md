# Development Progress Aligned To `D:\docs\development_order.md`

## Alignment Audit (2026-04-08)

- 结论：纵向打通颗粒度已基本对齐，主链按“可运行链路 + 测试证据”组织。
- [纰漏] 旧版 `Current Snapshot` 与 `Phase 3.1/4.5/6.5/7` 大量重复，快照和分阶段边界不清。
- [纰漏] 旧版 `Current Critical Path` 第 1 条仍停在“推进到自注册心跳”，与代码现状不符；已改为“高并发调度与故障恢复策略收口”。
- [纰漏] 旧版多处“已完成”未标注验证状态；现补充关键测试结果。
- [超额] `Phase 6.5` 已超出“统计型 RTCP”：`SR/LSR/DLSR RTT`、`NACK 重传`、`PLI cooldown`、`REMB 聚合/下发`均已实现并有测试覆盖。
- [超额] `Phase 7` 屏幕共享已超出“只发不收”：已落地反馈消费、码率重配置观测与自动化测试。

## Current Snapshot

- `Phase 0~3`：协议、登录、建会/入会/离会、会议状态同步主链可运行；剩余重点是 proto 全量审计与多节点语义收口。
- `Phase 3.1`：`NodeStatusMonitor` 已切到“配置节点 + Redis 注册节点”联合轮询，动态节点故障摘除/恢复路径补齐了测试证据。
- `Phase 3.1`：媒体发布注册已具备“失败节点隔离 -> 备节点 CreateRoom -> AddPublisher 重试 -> 路由回写”最小自动回补闭环。
- `Phase 3.1`：并发发布者失败恢复已加会议级串行化与“最新路由重试”去重，新增并发用例验证只触发一次恢复建房。
- `Phase 3.1`：半失败回滚已补齐：`CreateRoom` 成功但 `AddPublisher` 失败时会执行 `DestroyRoom` 清理，避免候选节点残留孤儿房间。
- `Phase 3.1`：新增 `MediaRouteStatusNotify(0x0306)` 状态事件，回补成功/失败都会广播并在 Qt `MeetingController` 落地提示。
- `Phase 3.1`：`MediaRouteStatusNotify` 已升级为 `switching/switched/failed` 结构化语义（JSON in `reason`），服务端 3s 去重 + 客户端终态提示抑噪已补齐测试。
- `Phase 3.1`：新增会议级 Redis 分布式恢复锁（`sfu_recovery_lock:{meeting_id}`），同一会议跨信令节点只允许单点回补，其它节点改为短等待后跟随重试，减少并发重复切换。
- `Phase 3.1`：恢复锁参数已配置化（`SIGNALING_SFU_RECOVERY_LOCK_TTL` / `SIGNALING_SFU_RECOVERY_FOLLOWUP_DELAY`），并补 `config` 层默认值/覆盖值/非法值回退测试。
- `Phase 3.1`：恢复链路新增 Redis 指标埋点（锁尝试/竞争、跟随重试、回补成功失败），并在 handler/store 测试中校验关键计数，支撑后续压测调参。
- `Phase 3.1`：新增按 SFU 节点聚合的恢复计数（attempt/success/failed）；`SFUNodeStatus` 查询已合并返回运行态+恢复态，复用现有节点状态读取通道做观测展示（无协议变更）。
- `Phase 3.1`：新增可选管理接口 `GET /admin/sfu/nodes`（`SIGNALING_ADMIN_LISTEN_ADDR` 控制开关），返回“配置/注册节点 + 恢复指标节点”的统一快照，含运行态与恢复态计数。
- `Phase 3.1`：`/admin/sfu/nodes` 已追加 `signaling_recovery` 快照（按当前 `SIGNALING_NODE_ID` 聚合锁竞争/跟随重试/回补计数），单接口可同时观测“信令侧恢复行为 + SFU 节点态”。
- `Phase 3.1`：`/admin/sfu/nodes` 已支持 `node_id` 过滤（支持逗号分隔/重复参数），可按节点精确拉取快照以降低管理端查询开销。
- `Phase 3.1`：`/admin/sfu/nodes` 已支持 `include_signaling=false`，可在只关注 SFU 节点态时跳过信令恢复快照字段，减少无关负载。
- `Phase 3.1`：恢复切路由已改为 CAS（`SwitchRoomRoute`）语义，仅更新 `sfu_node_id/sfu_route`，避免回写时覆盖 `host_id/mute_all`；并补“期望节点不匹配拒绝覆盖”与“reroute 后 mute_all 保真”测试。
- `Phase 3.1`：CAS 冲突分支已增强：并发已切路由时会按最新路由重试发布；若当前路由不在候选节点则回滚临时房间，避免遗留孤儿房间；恢复不再依赖 `host_id` 元数据。
- `Phase 3.1`：`MediaRouteStatusNotify` 去重窗口已配置化（`SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW`），支持按压测结果调节提示节流灵敏度，并补 `config` + handler 去重窗口测试。
- `Phase 3.1`：新增路由状态节流可观测指标（`route_status_emitted` / `route_status_deduped`）并并入 `signaling_recovery` 快照，支持直接评估去重窗口参数效果。
- `Phase 3.1`：路由状态去重缓存已加惰性清理（按窗口阈值清扫 stale meeting entry），避免长期运行下 `routeStatusCache` 无界增长，并补覆盖测试。
- `Phase 4.5`：本机双实例 P2P 音频闭环稳定，最小 RTCP `SR -> RR -> RTT` 已运行，RR 已驱动音频发送码率调整。
- `Phase 6`：Go↔SFU 发布者注册、按 `sfu_route` 路由调用、`SfuDaemon` 与主动上报链路已打通。
- `Phase 6.5`：RTCP 已从“统计”推进到“可执行恢复/控制”（NACK/PLI/REMB + RTT 回算 + SFU 下发估计 REMB）。
- `Phase 6.6 + 7`：屏幕共享已具备反馈驱动发布端行为与观测指标，当前卡点是多人订阅模型、tile 编排、长时稳定性。
- 本地关键验证（`Debug`）已通过：`sfu_core_tests`、`sfu_rpc_server_tests`、`sfu_daemon_tests`、`test_screen_share_session`、`test_meeting_runtime_smoke`、`audio_codec_smoke`。

## Phase 0 - Shared Protocol Layer + Skeleton

### 0.1 Protobuf 协议定义

- 当前状态：`partial`
- 已完成：`MediaOffer/Answer` 已携带 `audio_ssrc/video_ssrc`，Go/Qt/SFU 生成物已同步。
- 差距：仍需按 `api_interfaces*.md` 做消息全集、编号区间、向后兼容性审计；当前工作区 proto 相关改动需统一收口。

### 0.2 Go 信令服务器骨架

- 当前状态：`done`

### 0.3 Qt 客户端骨架

- 当前状态：`mostly_done`
- 差距：仍需结合实际构建与运行确认基础模块从“存在”提升到“full-plan”。

### 0.4 二进制帧编解码

- 当前状态：`done`

## Phase 1 - Go Signaling Core

### 1.1 TCP 网络层

- 当前状态：`done`

### 1.2 消息路由

- 当前状态：`done`

### 1.3 存储层

- 当前状态：`partial`
- 已完成：`MeetingLifecycleStore` 已接 `MeetingRepo + ParticipantRepo`；Redis 房间总线/节点直投/会话目录最小链路已打通。
- 差距：需补更完整 Redis 行为测试与更高层业务广播路径。

### 1.4 认证模块

- 当前状态：`partial`
- 已完成：跨节点重复登录踢旧会话主路径已补齐。
- 差距：需继续核验 Argon2id/限流/断线清理与 production path 覆盖。

## Phase 2 - Qt Client Login + Connection

### 2.1 SQLite 数据库初始化

- 当前状态：`done`

### 2.2 安全与 Token 模块

- 当前状态：`mostly_done`
- 差距：需与自动登录和持久化路径继续联调。

### 2.3 信令客户端网络层

- 当前状态：`mostly_done`
- 已完成：protobuf 生成链路已恢复可构建。
- 差距：TLS 握手与状态机/持久化联动仍未收口。

### 2.4 应用层控制

- 当前状态：`mostly_done`
- 差距：`EventBus` 未实现（当前主路径不依赖）。

### 2.5 登录页 UI

- 当前状态：`done`

## Phase 3 - Meeting Management

### 3.1 Go 信令侧

- 当前状态：`partial`
- 已完成：
  - repo-backed `MeetingHandler/Router` 主路径可用，`Create/Join/Leave/Kick/MuteAll` 最小失败语义已统一。
  - `room:{meeting_id}` / `sfu_route:{meeting_id}` / `room_member:*` 路径已接通，`Join/AddPublisher/RemovePublisher/DestroyRoom` 可按 route 闭环。
  - 多 SFU 节点池、健康/容量感知分配、`GetNodeStatus` 轮询、`ReportNodeStatus/QualityReport` 主动上报已接入 Redis。
  - `NodeStatusMonitor` 已支持“静态配置 + 动态注册”联合轮询，新增 `sfu-b` 与“无静态配置仅注册节点”场景测试。
  - `MediaHandler.registerPublisher` 新增失败后恢复路径：失败节点隔离、备节点重建房间、发布者重试、`room/sfu_route` 回写切换。
  - `MediaHandler` 新增按会议粒度恢复锁，避免并发回补重复建房；恢复前先按最新路由重试发布者注册以吸收已完成切换。
  - 回补流程新增半失败回滚：候选节点建房后若发布失败，立即发起 `DestroyRoom` 撤销临时房间并隔离该节点。
  - 回补可观测性已补齐：服务端广播 `MediaRouteStatusNotify`，客户端接收后更新状态文案并输出 `infoMessage`。
  - `SfuDaemon` 常驻启动骨架已可提供 RPC + UDP 服务。
  - 回补链路切路由由 `UpsertRoom` 升级为 CAS `SwitchRoomRoute`，仅改写 `sfu_node_id/sfu_route`，避免重置 `mute_all` 等房间状态；新增 stale expected node 拒绝覆盖测试与 reroute 保真测试。
  - CAS 冲突（期望节点失配）时改为“最新路由重试 + 非目标候选节点临时房间回滚”，并补“hostless 元数据仍可恢复”与“并发切路由回滚+重试”测试。
  - `MediaRouteStatusNotify` 去重窗口从固定 3s 升级为配置项 `SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW`，默认 3s；新增默认值/覆盖值/非法值回退测试与窗口到期后再次发出的 handler 测试。
  - 路由状态通知新增 emitted/deduped 计数，已接入 Redis 恢复指标与 `/admin/sfu/nodes` 的 `signaling_recovery` 输出，并补 store/handler/admin 覆盖测试。
  - `routeStatusCache` 新增周期性 stale-entry 清理，防止会议规模增长时缓存只增不减。
- 已完成：状态事件分级语义（`switching/switched/failed`）与 UI 展示降噪策略已落地（切换中只更新状态栏，终态短窗口去重提示）。
- 已完成：恢复流程已接入会议级分布式锁，跨节点并发回补时可避免重复建房/重复切路由。
- 差距：需在高频抖动故障下继续校准状态提示节流窗口（当前 3s）与阶段化文案策略。

### 3.2 Qt 客户端侧

- 当前状态：`mostly_done`
- 已完成：活动共享者控制态、`video-only` 订阅、音视频分离协商状态、远端订阅过滤已可运行。
- 差距：仍是“单音频目标 + 单活动共享者”上限模型，未到完整多人订阅。

## Phase 4 - Audio Pipeline

### 4.1 音频采集

- 当前状态：`partial`
- 已完成：默认输入设备链路 + 统一 `float mono/48kHz/20ms` 帧。
- 差距：设备枚举/切换 UI 未完成。

### 4.2 Opus 编解码

- 当前状态：`partial`
- 已完成：主路径可运行，最小自适应码率已接入。
- 差距：参数调优与观测收口不足。

### 4.3 RTP 传输

- 当前状态：`partial`
- 已完成：最小 P2P 语音收发闭环；`SR/RR/RTT` 运行态成立。
- 差距：NACK/PLI 与更完整 BWE 输入仍待补齐。

### 4.4 音频播放

- 当前状态：`partial`
- 已完成：线程化播放核心 + 默认输出设备接入。
- 差距：设备切换与运行态参数暴露不足。

### 4.5 集成联调

- 当前状态：`partial`
- 已完成：`Signaling -> MediaSessionManager -> AudioCallSession -> RTP/UDP -> JitterBuffer -> Decoder -> Player` 主链可运行，关键 smoke/test 已建立。
- 差距：设备切换、长时稳定性、体验侧问题清单仍待收口。

## Phase 5 - Video Pipeline

- 5.1 摄像头采集：`stub_or_partial`
- 5.2 H.264 硬件编码：`stub_or_partial`
- 5.3 RTP 视频封包：`pending`
- 5.4 H.264 硬件解码：`stub_or_partial`
- 5.5 OpenGL NV12 渲染：`stub_or_partial`
- 5.6 音视频同步 + 集成：`stub_or_partial`

## Phase 6 - C++ SFU Core

### 6.1 UDP 网络层

- 当前状态：`partial`

### 6.2 信令 RPC 通道

- 当前状态：`done_for_current_scope`
- 已完成：Go→SFU 发布者注册桥接闭环，protobuf wire 兼容修复，RPC server tests 覆盖主链。

### 6.3 房间管理

- 当前状态：`done_for_skeleton`

### 6.4 RTP 路由

- 当前状态：`mostly_done`
- 已完成：`AddPublisher -> MediaIngress -> UdpServer -> RtpRouter` 主链打通，发布者 SSRC 原位重绑支持。
- 差距：仍需更高并发与复杂拓扑稳定性验证。

### 6.5 RTCP + NACK

- 当前状态：`partial`
- 已完成：
  - RR/SR 解析、24-bit signed `cumulative_lost` 修正、RTCP 判定防误分类。
  - `SR/LSR/DLSR` RTT 回算已接入质量快照。
  - Generic NACK 重传、PLI 转发与 cooldown、REMB 转发与多订阅者最小值聚合。
  - `SendEstimatedRembToPublisher` 已接入 `SfuDaemon`，推荐码率可下行反馈到发布端。
- 差距：需继续扩大到高并发与复杂订阅拓扑，并量化体验收益。

### 6.6 客户端改造 + 联调

- 当前状态：`partial`
- 已完成：`ScreenShareSession` 已消费 NACK/PLI/REMB，支持发送缓存重传、关键帧请求计数、目标/已应用码率与应用时延观测。
- 差距：反馈驱动仍停留在单路屏幕共享会话，未扩到完整多人发布模型。

## Phase 7 - Enhanced Features

- 聊天持久化 / FTS5：`partial`
- 屏幕共享：`partial`
  - 已完成：采集、编码、RTP 发送、FU-A 重组、解码渲染、反馈驱动与观测测试的单路闭环。
  - 差距：多人订阅模型、tile 编排、长时稳定性验证。
- NAT 穿透 / STUN / TURN：`stub_or_partial`
- 动态 BWE：`partial`
  - 已完成：SFU 侧 `loss+rtt+jitter` 推荐码率与音频 RR 驱动码率调整。
  - 差距：视频侧 BWE/降级联动与长期参数调优。
- AEC / NS / AGC：`stub_or_partial`
- 文件传输 / 录制 / 滤镜：`pending`

## Current Critical Path

1. 把 SFU 调度从“最小可用自注册/心跳”推进到“生产可用”：重点补故障摘除后的自动回补、跨节点一致性与高并发压测口径。
2. 把 NACK/PLI/REMB 能力从“功能可用”推进到“复杂拓扑稳定”：补高并发、多发布者/多订阅者场景下的长期稳定性和收益量化。
3. 把 Qt 屏幕共享从“单活动共享者闭环”推进到“真正多人可用”：明确远端订阅模型、tile 组织、交互态管理。
4. 收口音频运行态体验：设备切换、长时稳定性、参数调优；并将 BWE 与降级策略扩展到视频侧。
5. 完成 proto 全量兼容审计，收敛当前分散改动为一次可追踪协议升级。

## Agent Dispatch (Condensed)

- Proto Agent：收口 `signaling.proto/sfu_rpc.proto` 编号与兼容性审计，统一生成代码。
- Go-Sig Agent：收口节点调度、故障回补、多节点语义与 Redis 房间态一致性。
- Qt-UI Agent：推进多人订阅模型与 tile 编排，补设备切换/状态提示。
- AV-Eng Agent：收口音频长稳与自适应参数，准备视频侧 BWE/降级联动。
- Cpp-SFU Agent：推进 RTCP 反馈链路在高并发复杂拓扑下的稳定性与收益验证。
