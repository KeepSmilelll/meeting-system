# Development Progress

> 当前文件只记录 2026-04-25 后的阶段性结论；历史进度已归档到 `D:\docs\audit\development_progress*.md`。

## Current Snapshot

当前阶段标签：`ubuntu-single-node-deploy`

核心结论：
- 单节点 SFU + ICE-Lite/DTLS-SRTP 的本机媒体主链路已打通，但仍不是生产可用多人会议。
- P0-A 已完成：审计基线已区分代码实现完成度与产品/部署就绪度。
- Phase 2/3 不再按满完成度记录：登录、信令和会议 handler 基础可用，Qt 通过会议号入会入口已补齐最小可用弹窗。
- 协议层代码语义以 client-to-SFU transport 协商为准；主要接口文档中的旧 peer-targeted `MEDIA_OFFER/ANSWER/ICE` 转发描述已清理。

## Active Work

- P0-A：完成 `module_progress_audit.md`、`development_progress.md` 和媒体协商文档一致性更新。
- P0-B：完成 `JoinMeetingDialog.qml` 最小闭环，接入 `MeetingController.joinMeeting()`，支持会议号校验、可选密码、loading 和取消。
- P0-C：已完成协议、candidate 收集和媒体 socket relay 接入：`StunClient` 支持 Binding/XOR-MAPPED-ADDRESS，`TurnClient` 支持 Allocate/CreatePermission/Send/Data indication 的构建/解析，Qt join response 已透传 `ice_servers`，transport offer 会按 `MEETING_ICE_POLICY=all|relay-only` 收集 host/srflx/relay candidates。
- P0-C：`UdpPeerSocket` 已支持 TURN relay 模式；`relay-only` 下音频/视频 session 会在 transport answer 后执行 TURN Allocate/CreatePermission，后续 DTLS/SRTP/RTP/RTCP 出站包包装为 Send indication，入站 Data indication 解包后交给现有媒体管线。
- P0-C：已完成本机双实例验证入口扩展；`run_meeting_client_dual_ui_smoke.ps1` 和 `run_m2_dual_instance_validation.ps1` 支持注入 `MEETING_ICE_POLICY`、`SIGNALING_TURN_SERVERS`、`SIGNALING_TURN_SECRET`、`SIGNALING_TURN_REALM`，`meeting-server/turnserver.conf` 已补成本机 coturn REST secret 配置。
- P0-C：当前机器无法直接安装/运行官方 coturn：`turnserver`、`coturn`、`docker` 均不在 PATH，WSL 功能未启用且提示需要 `wsl --install`。已在 `D:\meeting-tools\local-turn-server` 部署本机最小 TURN relay 工具用于 relay-only E2E 验证；该工具只覆盖 Allocate/CreatePermission/Send/Data indication 的本机验证子集，不替代生产 coturn。
- P0-C：修复 relay-only 验证中发现的客户端竞态：relay-only 下不再先走直连 `setPeer(SFU)` 快路径，TURN setup 阶段通过 mutex 防止媒体 recv 线程抢走 Allocate/CreatePermission 响应，TURN 控制等待只接受来自 TURN server 且 transaction id 匹配的响应。
- P0-D：已补齐 Ubuntu 单机 Docker Compose 部署包，覆盖 signaling、SFU、MySQL、Redis、coturn；目标云主机公网地址按 `123.207.41.63` 写入 `.env.example` 和部署文档。

## Verification Notes

- 已执行：QML import 扫描，无旧版 `import QtQuick 2.x`。
- 已执行：Go 信令 `go test ./... -v -count=1` 通过。
- 已执行：Qt 客户端 `cmake --build build --target meeting_client --config Debug` 通过。
- 已执行：Qt CTest `ctest -C Debug --output-on-failure` 26/26 通过，其中 2 项摄像头 smoke 按环境跳过。
- 已执行：新增 `test_stun_turn_client` 覆盖 STUN Binding、TURN Allocate/CreatePermission/Send/Data indication 协议路径，以及 `UdpPeerSocket` 经本地假 TURN server 的 Send/Data indication 包装/解包。
- 已执行：本机双实例 baseline `run_meeting_client_dual_ui_smoke.ps1 -SyntheticAudio -SyntheticCamera -RequireAudio -RequireVideo -RequireAvSync -Headless -TimeoutSeconds 90` 通过。
- 已执行：完整 M2 本机双实例 `run_m2_dual_instance_validation.ps1 -SyntheticAudio -SyntheticCamera -RequireAudio -RequireVideo -RequireAvSync -UiRequireAvSync -Headless -UiTimeoutSeconds 90 -SoakMinutes 0` 通过；CPU 平均约 host 9.05%、guest 8.90%，峰值约 host 19.25%、guest 18.37%。
- 已执行：脚本改动后本机双实例 `run_meeting_client_dual_ui_smoke.ps1 -SyntheticAudio -SyntheticCamera -RequireAudio -RequireVideo -Headless -TimeoutSeconds 90` 通过；强制 AVSync 的单次复跑出现 host AVSync 样本数不足 10 的既有抖动，但音频、视频、ICE、DTLS、SRTP 证据均已出现。
- 已执行：`run_meeting_client_dual_ui_smoke.ps1 -IcePolicy relay-only` 在未提供 `-TurnServers` 时会明确报错，避免误跑成非 relay-only 验证。
- 已执行：`D:\meeting-tools\local-turn-server` 执行 `go test .` 通过。
- 已执行：本机 TURN relay-only 双实例 `run_meeting_client_dual_ui_smoke.ps1 -SyntheticAudio -SyntheticCamera -RequireAudio -RequireVideo -RequireAvSync -Headless -IcePolicy relay-only -TurnServers "turn:127.0.0.1:3478?transport=udp" -TurnSecret "local-turn-secret" -TurnRealm "meeting.local" -TimeoutSeconds 120` 通过，host/guest 均出现 audio/video/AVSync 证据。
- 已执行：Qt 客户端回归 `ctest -C Debug --output-on-failure` 26/26 通过，其中 2 项摄像头 smoke 按环境跳过。
- 已执行：部署文件离线生成完成：`meeting-server/docker-compose.yml`、signaling/SFU Dockerfile、`.env.example`、Ubuntu 单机部署文档和检查脚本。
- 未执行：本机没有 Docker CLI，无法在当前 Windows 环境运行 `docker compose config` 或镜像构建；需要在腾讯云 Ubuntu 主机上执行部署验证。
