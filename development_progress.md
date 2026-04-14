# Development Progress

> 最后更新：2026-04-14 | 对齐文档：`docs/development_order.md`

## 主链打通度

```
Phase 0  协议+骨架     ██████████░  90%   done / mostly_done
Phase 1  Go 信令核心   ████████░░░  80%   done (认证已升级 Argon2id+JWT)
Phase 2  Qt 登录连接   ████████░░░  80%   done (安全模块已实现)
Phase 3  会议管理      ████████░░░  75%   partial (持久化 repo 已实现,接入中)
Phase 4  音频管线      ███████░░░░  65%   partial (链路可运行,双端联调未完成)
Phase 5  视频管线      ██████░░░░░  60%   partial (硬编解码已接入,单路闭环)
Phase 6  SFU           ██████░░░░░  55%   partial (转发链路成立,多人未联调)
Phase 7  增强功能      ███░░░░░░░░  25%   屏幕共享超额,其余待做
```

**当前最远到达点**：登录 → 建会/入会/离会 → 状态同步 → P2P 音频协商 → 屏幕共享反馈闭环 → 单路真实摄像头视频协商/RTP/远端解码渲染

---

- 2026-04-13（收口推进）已完成 M1 第 1/2 项：补齐麦克风/扬声器设备枚举与切换链路（AudioCapture/AudioPlayer/AudioCallSession/MeetingController/QML ToolBar 全链路），新增音频输入/输出偏好持久化（SettingsRepository + UserManager）并在会中支持热切换；`ctest -C Debug --output-on-failure -R "test_database|test_meeting_runtime_smoke|test_meeting_client_process_smoke"` 已实测通过。
- 2026-04-13（收口推进）已完成 M1 第 4 项：RTCP RTT 与音频 BWE 指标接入运行时验收（MeetingController 暴露 audioLastRttMs/audioTargetBitrateBps，runtime smoke 新增 RTT>0 与目标码率范围断言）；ctest -C Debug --output-on-failure -R test_meeting_runtime_smoke 与 test_meeting_client_process_smoke 通过，go test ./auth ./store -count=1 通过。
- 2026-04-13（收口推进）已完成 M1 第 3 项（同机双实例真实音频联调）：新增 real-audio smoke 收口门禁（RuntimeSmokeDriver 支持 MEETING_SMOKE_REQUIRE_AUDIO，要求 sent/recv/played + RTT + 目标码率证据）并新增 test_meeting_runtime_real_audio_smoke；ctest -C Debug --output-on-failure -R "test_meeting_client_real_audio_smoke|test_meeting_runtime_real_audio_smoke"（沙箱外）已通过。
- 2026-04-13（收口推进）已落地 M1 第 5 项执行入口：`RuntimeSmokeDriver` 新增 `MEETING_SMOKE_SOAK_MS` soak 阶段（证据达成后可持续在会运行），process smoke 新增 `MEETING_PROCESS_SMOKE_SOAK_MS`/超时联动与 WorkingSet 增长门禁，并新增脚本 `scripts/run_m1_audio_leak_soak.ps1`（默认 30min，可选 `-EnableAsanLeakChecks`）；短时 dry-run（15s soak）+ 脚本化 1min soak（合成音频与真实音频各 1 次：`scripts/run_m1_audio_leak_soak.ps1 -DurationMinutes 1 -SyntheticAudio` / `scripts/run_m1_audio_leak_soak.ps1 -DurationMinutes 1`）+ 完整 30min 真实音频 soak（`scripts/run_m1_audio_leak_soak.ps1 -DurationMinutes 30`，1814.88s）均已通过。
- 2026-04-14（Ubuntu 适配）已创建分支 `codex/ubuntu-m1-adapt`：`AudioCallSession` 从 Win-only 改为 Win/Linux 双平台 UDP socket 实现（RTCP RTT/BWE 路径保留），`runAudioCallSessionLoopbackSelfCheck` 开放 Linux；客户端 CMake 增加系统路径 FFmpeg 查找（非 Windows 不再依赖固定 `D:/...` hint）。Windows 回归：`ctest --test-dir plasma-hawking/build -C Debug --output-on-failure -R "test_meeting_runtime_smoke|test_meeting_client_process_smoke|audio_codec_smoke"` 已验证通过（runtime/process 在沙箱外通过）。

## Phase 0 — 协议 + 骨架

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 0.1 Protobuf | `partial` | MediaOffer/Answer 已携带 SSRC。差距：消息全集/编号审计未做 |
| 0.2 Go 骨架 | `done` | main.go / config / logger / snowflake 已实现 |
| 0.3 Qt 骨架 | `mostly_done` | RAII Wrappers / RingBuffer 已实现。差距：Types.h/Config 仍为空壳 |
| 0.4 帧编解码 | `done` | 双端 frame round-trip 测试通过 |

## Phase 1 — Go 信令核心

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 1.1 TCP 网络层 | `done` | Session 三 goroutine / SessionManager 单点踢除 |
| 1.2 消息路由 | `done` | Router + 状态中间件 |
| 1.3 存储层 | `partial` | MeetingLifecycleStore 已接 MeetingRepo + ParticipantRepo；Redis 房间态已通。差距：message_repo 空壳 |
| 1.4 认证模块 | `done` | Argon2id + JWT(HS256) + Redis 会话 + 限流 + 断线清理 |

## Phase 2 — Qt 登录 + 连接

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 2.1 SQLite | `done` | WAL / FK / 8表 + FTS5 / test_database 通过 |
| 2.2 安全模块 | `done` | TokenManager + CryptoUtils 已实现 |
| 2.3 信令客户端 | `mostly_done` | 连接/重连/心跳/帧收发可运行。差距：TLS |
| 2.4 应用层 | `mostly_done` | UserManager + AppStateMachine + MeetingController。差距：EventBus 空壳 |
| 2.5 登录 UI | `done` | LoginDialog + HomePage + TitleBar |

## Phase 3 — 会议管理

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 3.1 Go 侧 | `partial` | Create/Join/Leave/Kick/MuteAll + Redis 房间 + TURN 凭证 + SFU RPC。差距：Repo 未完全接入主路径 |
| 3.2 Qt 侧 | `mostly_done` | MeetingController + ParticipantListModel + 建会/入会 UI + 通话记录 |

## Phase 4 — 音频管线

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 4.1 采集 | `partial` | 默认设备 48kHz/f32/Mono 可用。差距：设备枚举/切换 UI |
| 4.2 Opus | `partial` | 编解码主路径和码率调节可运行 |
| 4.3 RTP | `partial` | P2P 收发 + SR/RR/RTT 闭环。差距：NACK/PLI 不完整 |
| 4.4 播放 | `partial` | AudioPlayer 播放线程可运行。差距：设备切换 |
| 4.5 联调 | `partial` | 合成音频链路 smoke 通过。**差距：真实双端麦克风/扬声器联调未完成** |

## Phase 5 — 视频管线

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 5.1 摄像头采集 | `partial` | CameraCapture 可用 + 真实摄像头(iVCam) smoke 通过 |
| 5.2 硬件编码 | `done` | nvenc→amf→qsv→libx264 探测链 + sws_scale 已消除 |
| 5.3 RTP 视频 | `partial` | FU-A 分片/重组可用 |
| 5.4 硬件解码 | `done` | av_hwdevice_ctx_create + hw_frame_transfer + fallback |
| 5.5 OpenGL 渲染 | `partial` | NV12 Shader + VAO + 纹理上传已落地 |
| 5.6 AVSync | `partial` | 多级渲染策略(丢帧/延迟/PTS重排/节拍约束)。差距：完整音频主时钟调度器 |

## Phase 6 — SFU

| 子阶段 | 状态 | 说明 |
|--------|------|------|
| 6.1 UDP 网络 | `partial` | Boost.Asio 异步收发可运行 |
| 6.2 RPC 通道 | `done` | Go↔SFU CreateRoom/DestroyRoom/AddPublisher/RemovePublisher + QualityReport |
| 6.3 房间管理 | `done` | RoomManager + Room + Publisher + Subscriber |
| 6.4 RTP 路由 | `mostly_done` | MediaIngress → RtpRouter → SSRC 重写转发 |
| 6.5 RTCP+NACK | `partial` | SR/RR/RTT + NACK重传 + PLI cooldown + REMB 聚合(超额) |
| 6.6 客户端联调 | `pending` | **未开始：客户端未改为全连 SFU，多人未联调** |

## Phase 7 — 增强功能

| 功能 | 状态 |
|------|------|
| 聊天 + FTS5 | `partial` — 广播可用,SQLite 表已建,服务端 message_repo 空壳 |
| 屏幕共享 | `partial` — 超额：采集/编码/RTP/解码/渲染/NACK/PLI/REMB 反馈闭环 |
| NAT 穿透 | `stub` — TURN 凭证可下发,StunClient/TurnClient 空壳 |
| 动态 BWE | `partial` — 音频 RR 驱动码率,视频侧降级未做 |
| AEC/NS/AGC | `stub` |
| 文件传输 | `partial` — file_handler.go 已实现 |
| 录制/滤镜 | `pending` |

---

## 验证证据

| 测试 | 最后通过日期 | 环境 |
|------|-------------|------|
| `go test ./...` (signaling) | 2026-04-09 | Ubuntu |
| `ctest` (sfu/build_sfu_check) | 2026-04-09 | Ubuntu |
| test_screen_share_session | 2026-04-09 | Windows |
| test_meeting_controller_mute_all | 2026-04-09 | Windows |
| test_meeting_runtime_smoke | 2026-04-11 | Windows |
| test_meeting_client_process_smoke | 2026-04-11 | Windows |
| test_database | 2026-04-11 | Windows (沙箱外) |
| test_av_sync | 2026-04-11 | Windows |
| 真实摄像头 one-way smoke | 2026-04-11 | Windows + iVCam |

---

## Agent Dispatch

| Agent | 当前优先任务 |
|-------|-------------|
| **Proto** | 消息全集/编号审计,确保与 api_interfaces.md 对齐 |
| **Go-Sig** | MeetingRepo/ParticipantRepo 接入 Handler 主路径 |
| **Qt-UI** | 多远端流模型 + VideoGrid 宫格编排 |
| **AV-Eng** | 真实双端 P2P 音视频联调 (M1→M2 闭合) |
| **Cpp-SFU** | 客户端改连 SFU + 3人联调 (M3 闭合) |








