# 模块审计异议与后续推进计划

## Summary

我基本认同 `module_progress_audit.md` 的主结论：当前核心闭环是“本机单节点 SFU + ICE-Lite/DTLS-SRTP + 双/三实例 smoke”，最大缺口是客户端 Full ICE/TURN、UI 空壳和部署层。

我有 3 点补充修正：

- Phase 2 `100%` 偏高：`JoinMeetingDialog.qml`、`SettingsPage.qml`、`SidePanel.qml` 仍是空 `Item {}`，只能说登录/信令基础完成，产品入口未完成。
- Phase 3 `95%` 偏高：后端会议管理基本完成，但“用户通过会议号加入会议”的 Qt UI 入口为空壳，应按 `85%` 看。
- 协议层代码可给 `95%`，但文档仍有旧语义残留：部分接口表还写 `MEDIA_OFFER/ANSWER` 为“转发”，`server_design.md` 早期路由段仍提 `target_user_id`。这是文档一致性问题，不是代码主链路问题。

## Key Changes

### P0-A：修正审计基线与文档一致性

- 更新 `module_progress_audit.md` / `development_progress.md`：区分“代码实现完成度”和“产品/部署就绪度”。
- 清理文档旧媒体语义：`MEDIA_OFFER/ANSWER/ICE` 统一描述为 client-to-SFU transport 协商，不再写 peer-targeted 转发。
- 明确当前阶段标签：`single-node-sfu-e2e`，不是生产可用多人会议。

### P0-B：补齐用户入会入口

- 实现 `JoinMeetingDialog.qml`：会议号、密码、错误提示、loading 状态、回车提交、取消关闭。
- 接入现有 `MeetingController.joinMeeting()` / join result 信号，成功后进入会议房间。
- 会议号校验固定为 6-8 位数字；密码允许为空。
- smoke 新增“非 host 通过 UI join path 入会”的覆盖，不再只依赖 runtime env 自动入会。

### P0-C：实现客户端 Full ICE / TURN 最小闭环

- 实现 `StunClient`：STUN Binding、XOR-MAPPED-ADDRESS、transaction id 校验、超时重试。
- 实现 `TurnClient`：Allocate、CreatePermission、Send/Data indication 的最小 relay 流程。
- 新增 `IceAgent` 或等价控制器：收集 host/srflx/relay candidates，按 host > srflx > relay 优先级 trickle 给信令。
- 保留现有本机 host-candidate 快路径，但正式 transport offer 不再手写单 host candidate。
- 新增测试开关 `MEETING_ICE_POLICY=all|relay-only`；`relay-only` 用于强制走 TURN 验证。

### P1：补齐产品可用性和多人质量

- `SettingsPage.qml` 实现音频输入、音频输出、摄像头、ICE policy 配置，并写入现有 settings 存储。
- `ChatPanel.qml` 接入现有 Go `ChatHandler`：发送、接收、SQLite 保存、会议内消息列表。
- SFU 实现下一步 LayerSelector/Simulcast 占位到可运行：先支持单路上行、多订阅者带宽降级策略，后续再做多层编码。
- 完整化 `docker-compose.yml`：signaling、sfu、redis、mysql、coturn；先不做 Nginx TLS 生产化。

## Test Plan

- 协议/信令：`go test ./... -count=1`，覆盖 `SetupTransport`、`TrickleIceCandidate`、join response `ice_servers`。
- Qt 单元：新增 STUN/TURN/ICE candidate 收集测试；保留 `test_dtls_transport_client`、`test_security_utils`。
- SFU：`ctest -C Debug --output-on-failure`，确认裸 RTP 拒绝、DTLS-SRTP 后转发、fanout 不回归。
- UI：QML import/qmllint；Join dialog 手动输入会议号加入的 runtime smoke。
- E2E：
  - LAN host candidate：双实例和三实例 smoke 必须继续通过。
  - `MEETING_ICE_POLICY=relay-only`：通过本地 coturn 完成双实例音视频闭环。
  - 真实设备：继续使用 `DroidCam Video`、`e2eSoft iVCam`、`EDIFIER Halo Soundbar` 作为本机验证矩阵。

## Assumptions

- 下一阶段仍只保证单节点 SFU，不做多节点迁移和故障切换。
- 不修改 protobuf 字段，现有 `MediaIceCandidate` 已足够承载 trickle ICE。
- TURN 使用 coturn，不自研 TURN server。
- 本机真实音频设备只有 `Microphone (EDIFIER Halo Soundbar)` 和 `扬声器 (EDIFIER Halo Soundbar)`；多人音频测试中其余发布者默认 synthetic audio。
