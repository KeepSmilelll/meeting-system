# 横向收口计划 (2026-04-13)

> **推进方向**：横向收口，闭合三个里程碑
> **前置审计**：P0 铁律违反已全部修复，空壳文件已基本消除

---

## 当前形态与决策依据

纵向链已穿至 Phase 6（SFU 转发），但每层都是"单向 smoke 验证"。三个里程碑**一个都没闭合**：

| 里程碑 | 计划验证标准 | 实际状态 |
|--------|-------------|---------|
| 🎯 M1：两人 P2P 语音 | 真实双端通话,延迟 ≤200ms,30min 无泄露 | 合成音频 smoke 通过，真实双端未验证 |
| 🎯 M2：两人 P2P 音视频 | 双向摄像头,硬解+NV12,AVSync ±40ms,CPU ≤15% | 单路 one-way 通过，双向未验证 |
| 🎯 M3：三人 SFU 会议 | 3 人互看互听,NACK 5%丢包仍流畅,PLI ≤1s | 转发链路成立，客户端未改连 SFU |

**结论**：停止纵向推进 Phase 7，横向收口 M1→M2→M3。

---

## M1：两人 P2P 语音通话 (2 天)

### 任务

| # | 任务 | Agent | 产出 |
|---|------|-------|------|
| 1 | 音频设备枚举/切换 | AV-Eng | AudioCapture + AudioPlayer 支持 `availableDevices()` / `setDevice()` |
| 2 | 音频设置 UI | Qt-UI | `AudioSettings.qml` — 麦克风/扬声器下拉框 |
| 3 | 真实双端 P2P 语音联调 | AV-Eng | 两台机器（或同机双实例）真实麦克风/扬声器通话 |
| 4 | RTCP RTT 测量 | AV-Eng | 解析 SR → 回 RR → 计算 RTT 并上报 |
| 5 | 30min 内存泄露测试 | AV-Eng | Valgrind/AddressSanitizer 跑 30 分钟通话 |

> 进展（2026-04-14）：第 5 项已闭合：`RuntimeSmokeDriver` soak 阶段 + `test_meeting_client_process_smoke` 内存增长门禁 + `scripts/run_m1_audio_leak_soak.ps1` 已落地，且完整 30min 真实音频 soak 已通过（1814.88s）。

### 验收标准

- [ ] 两台设备互相听到对方说话（真实麦克风/扬声器）
- [ ] 端到端延迟 ≤ 200ms（局域网 ≤ 50ms）
- [ ] 切换麦克风/扬声器 → 立即生效
- [ ] RTCP RR 正确报告丢包率和 RTT
- [x] 30 分钟通话无内存泄露

---

## M2：两人 P2P 音视频通话 (3 天)

> 前置：M1 已闭合

### 任务

| # | 任务 | Agent | 产出 |
|---|------|-------|------|
| 1 | 双向摄像头视频 | AV-Eng | 两端同时编码+发送+接收+解码+渲染 |
| 2 | 硬解码验证 | AV-Eng | Task Manager 确认 GPU Video Decode > 0 |
| 3 | NV12 零拷贝渲染验证 | AV-Eng | 确认无 sws_scale，GL_RED+GL_RG 纹理路径 |
| 4 | AVSync 验证 | AV-Eng | 音频主时钟驱动视频帧选择，±40ms 内 |
| 5 | CPU 占用测量 | AV-Eng | 1080p@30fps 硬件加速时 CPU ≤ 15% |
| 6 | VideoGrid 1v1 布局 | Qt-UI | 2人宫格(2×1)正确显示 |

### 验收标准

- [ ] 两台设备双向看到对方摄像头画面 + 听到声音
- [ ] 硬件编解码生效 (GPU 占用可观测)
- [ ] 音视频同步，无明显唇音不同步
- [ ] CPU ≤ 15%

---

## M3：三人+ SFU 多人会议 (5 天)

> 前置：M2 已闭合

### 任务

| # | 任务 | Agent | 产出 |
|---|------|-------|------|
| 1 | 客户端改连 SFU | AV-Eng | 入会后根据 `sfu_address` 连接 SFU（不再 P2P 直连） |
| 2 | 多远端流模型 | AV-Eng | 为每个远端用户创建独立 VideoDecoder |
| 3 | VideoGrid 宫格 | Qt-UI | 自适应布局: 1×1 / 2×1 / 2×2 |
| 4 | SFU SSRC 订阅 | Cpp-SFU | 每个 Subscriber 正确重写 SSRC |
| 5 | NACK 实战验证 | Cpp-SFU | 模拟 5% 丢包，画面仍流畅 |
| 6 | PLI 实战验证 | Cpp-SFU | 中途加入，≤1s 内看到画面 |
| 7 | Go 侧 SFU 编排 | Go-Sig | CreateRoom/AddPublisher/RemovePublisher 主路径与 Repo 对齐 |
| 8 | 3 台设备联调 | 全体 | 3 台设备同时入会，互相看到听到 |

### 验收标准

- [ ] 3 人会议：每人看到另外 2 人画面 + 声音
- [ ] NACK 重传：5% 丢包仍有画面
- [ ] PLI：中途加入 ≤ 1s 看到画面
- [ ] SFU CPU 占用：3 人会议 < 10%

---

## Agent 分工总览

```
M1 (2天)                  M2 (3天)                  M3 (5天)
─────────────────────── ─────────────────────── ───────────────────────
AV-Eng: 设备枚举/联调    AV-Eng: 双向视频/AVSync   AV-Eng: 改连SFU/多远端流
Qt-UI:  AudioSettings    Qt-UI:  VideoGrid 1v1    Qt-UI:  VideoGrid 多人
                                                   Cpp-SFU: NACK/PLI 实战
                                                   Go-Sig: Repo 接入主路径
```

## 暂缓项 (M3 之后再做)

| 项目 | 原因 |
|------|------|
| Phase 7 增强功能 | 独立可交付，不阻塞里程碑 |
| Proto 消息全集审计 | 当前消息集足够支撑 M1-M3 |
| TLS 握手 | 安全加固，不影响功能验证 |
| Simulcast 分层 | M3 基础闭合后再做 |
| 音频 AEC/NS/AGC | 体验优化，不阻塞核心链路 |





