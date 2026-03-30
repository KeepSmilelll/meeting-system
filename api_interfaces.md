# 视频会议系统 — 接口定义文档 (Part 1: 协议层 + 信令 API)

> **适用范围**：客户端 (C++/Qt) ↔ Go 信令服务器 ↔ C++ SFU 媒体服务器

---

## 1. 二进制帧协议规范

### 1.1 帧结构

所有 TCP 信令消息均封装在以下二进制帧中（大端序）：

```
 0       1       2       3       4       5       6       7       8       8+N
┌───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────────┐
│ Magic (2B)    │ Ver   │ Type (2B)     │ Length (4B)           │ Payload   │
│ 0xAB  │ 0xCD  │ 0x01  │ Hi    │ Lo    │ B3  B2  B1  B0      │ (N bytes) │
└───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────────┘
```

| 字段 | 偏移 | 大小 | 说明 |
|------|------|------|------|
| Magic | 0 | 2B | 固定 `0xABCD`，用于帧同步校验 |
| Version | 2 | 1B | 协议版本，当前 `0x01` |
| Type | 3 | 2B | 信令类型，见 §1.2 |
| Length | 5 | 4B | Payload 字节数（大端序），最大 1MB |
| Payload | 9 | N | Protobuf 序列化的消息体 |

### 1.2 信令类型枚举

```protobuf
// 与 C++ 客户端和 Go 服务端共用
enum SignalType {
  // === 认证 (0x01xx) ===
  AUTH_LOGIN_REQ        = 0x0101;
  AUTH_LOGIN_RSP        = 0x0102;
  AUTH_LOGOUT_REQ       = 0x0103;
  AUTH_LOGOUT_RSP       = 0x0104;
  AUTH_HEARTBEAT_REQ    = 0x0105;
  AUTH_HEARTBEAT_RSP    = 0x0106;
  AUTH_KICK_NOTIFY      = 0x0107;   // 服务端→客户端：被踢下线

  // === 会议控制 (0x02xx) ===
  MEET_CREATE_REQ       = 0x0201;
  MEET_CREATE_RSP       = 0x0202;
  MEET_JOIN_REQ         = 0x0203;
  MEET_JOIN_RSP         = 0x0204;
  MEET_LEAVE_REQ        = 0x0205;
  MEET_LEAVE_RSP        = 0x0206;
  MEET_KICK_REQ         = 0x0207;
  MEET_KICK_RSP         = 0x0208;
  MEET_MUTE_ALL_REQ     = 0x0209;
  MEET_MUTE_ALL_RSP     = 0x020A;
  MEET_STATE_SYNC       = 0x020B;   // 服务端→客户端：全量状态推送
  MEET_PARTICIPANT_JOIN  = 0x020C;  // 通知：有人加入
  MEET_PARTICIPANT_LEAVE = 0x020D;  // 通知：有人离开
  MEET_HOST_CHANGED     = 0x020E;   // 通知：主持人变更

  // === 媒体协商 (0x03xx) ===
  MEDIA_OFFER           = 0x0301;
  MEDIA_ANSWER          = 0x0302;
  MEDIA_ICE_CANDIDATE   = 0x0303;
  MEDIA_MUTE_TOGGLE     = 0x0304;   // 开关摄像头/麦克风
  MEDIA_SCREEN_SHARE    = 0x0305;   // 屏幕共享状态变化

  // === 聊天 (0x04xx) ===
  CHAT_SEND_REQ         = 0x0401;
  CHAT_SEND_RSP         = 0x0402;
  CHAT_RECV_NOTIFY      = 0x0403;   // 服务端→客户端：收到新消息

  // === 文件 (0x05xx) ===
  FILE_OFFER_REQ        = 0x0501;
  FILE_OFFER_RSP        = 0x0502;
  FILE_ACCEPT_REQ       = 0x0503;
  FILE_CHUNK_DATA       = 0x0504;
  FILE_COMPLETE_NOTIFY  = 0x0505;
}
```

---

## 2. Protobuf 消息定义 (`signaling.proto`)

```protobuf
syntax = "proto3";
package meeting;
option go_package = "meeting-server/signaling/protocol/pb";

// ===================== 通用 =====================

message ErrorInfo {
  int32  code    = 1;   // 错误码 (见 §2.1)
  string message = 2;
}

// ===================== 认证模块 =====================

message AuthLoginReq {
  string username      = 1;
  string password_hash = 2;   // 客户端先 SHA-256，服务端再 Argon2id 比对
  string device_id     = 3;   // 设备标识（用于单点登录踢除）
  string platform      = 4;   // "windows" | "macos" | "linux"
}

message AuthLoginRsp {
  bool      success    = 1;
  ErrorInfo error      = 2;
  string    token      = 3;   // JWT Token (有效期 7 天)
  string    user_id    = 4;
  string    display_name = 5;
  string    avatar_url = 6;
}

message AuthLogoutReq {}
message AuthLogoutRsp {
  bool success = 1;
}

message AuthHeartbeatReq {
  int64 client_timestamp = 1;  // 客户端本地时间 (ms)
}
message AuthHeartbeatRsp {
  int64 server_timestamp = 1;
}

message AuthKickNotify {
  string reason = 1;   // "账号在其他设备登录" | "被管理员踢出"
}

// ===================== 会议控制 =====================

message MeetCreateReq {
  string title            = 1;
  string password         = 2;   // 可选会议密码（空=无密码）
  int32  max_participants = 3;   // 0=使用默认值(16)
}

message MeetCreateRsp {
  bool      success    = 1;
  ErrorInfo error      = 2;
  string    meeting_id = 3;      // 6-8 位数字会议号
}

message MeetJoinReq {
  string meeting_id = 1;
  string password   = 2;
}

message MeetJoinRsp {
  bool      success        = 1;
  ErrorInfo error          = 2;
  string    meeting_id     = 3;
  string    title          = 4;
  string    sfu_address    = 5;  // SFU 服务器地址 "ip:port"
  repeated IceServer ice_servers = 6;
  repeated Participant participants = 7;  // 当前参会者列表
}

message IceServer {
  string urls       = 1;   // "stun:ip:3478" 或 "turn:ip:3478"
  string username   = 2;   // TURN 临时凭证用户名
  string credential = 3;   // TURN 临时凭证密码
}

message Participant {
  string user_id      = 1;
  string display_name = 2;
  string avatar_url   = 3;
  int32  role         = 4;   // 0=参会者 1=主持人 2=联合主持
  bool   is_audio_on  = 5;
  bool   is_video_on  = 6;
  bool   is_sharing   = 7;
}

message MeetLeaveReq {}
message MeetLeaveRsp {
  bool success = 1;
}

message MeetKickReq {
  string target_user_id = 1;
}
message MeetKickRsp {
  bool      success = 1;
  ErrorInfo error   = 2;
}

message MeetMuteAllReq {
  bool mute = 1;   // true=全员静音 false=解除静音
}
message MeetMuteAllRsp {
  bool success = 1;
}

// --- 服务端推送通知 ---

message MeetStateSyncNotify {
  string meeting_id = 1;
  string title      = 2;
  string host_id    = 3;
  repeated Participant participants = 4;
}

message MeetParticipantJoinNotify {
  Participant participant = 1;
}

message MeetParticipantLeaveNotify {
  string user_id = 1;
  string reason  = 2;   // "主动离开" | "被踢出" | "网络断开"
}

message MeetHostChangedNotify {
  string new_host_id = 1;
  string new_host_name = 2;
}

// ===================== 媒体协商 =====================

message MediaOffer {
  string target_user_id = 1;  // 目标用户（P2P）或 "sfu"
  string sdp            = 2;  // SDP 描述
}

message MediaAnswer {
  string target_user_id = 1;
  string sdp            = 2;
}

message MediaIceCandidate {
  string target_user_id = 1;
  string candidate      = 2;   // ICE candidate 字符串
  string sdp_mid        = 3;
  int32  sdp_mline_index = 4;
}

message MediaMuteToggle {
  int32 media_type = 1;   // 0=音频 1=视频
  bool  muted      = 2;
}

message MediaScreenShare {
  bool sharing = 1;   // true=开始共享 false=停止共享
}

// ===================== 聊天 =====================

message ChatSendReq {
  int32  type        = 1;   // 0=文字 1=图片URL 2=文件引用
  string content     = 2;
  string reply_to_id = 3;   // 引用消息ID（可选）
}

message ChatSendRsp {
  bool      success    = 1;
  ErrorInfo error      = 2;
  string    message_id = 3;
  int64     timestamp  = 4;  // 服务端时间戳 (ms)
}

message ChatRecvNotify {
  string message_id   = 1;
  string sender_id    = 2;
  string sender_name  = 3;
  int32  type         = 4;
  string content      = 5;
  string reply_to_id  = 6;
  int64  timestamp    = 7;
}

// ===================== 文件传输 =====================

message FileOfferReq {
  string file_name = 1;
  int64  file_size = 2;
  string file_hash = 3;   // SHA-256
}

message FileOfferRsp {
  bool      success     = 1;
  ErrorInfo error       = 2;
  string    transfer_id = 3;   // 传输会话 ID
}

message FileAcceptReq {
  string transfer_id = 1;
  bool   accepted    = 2;
}

message FileChunkData {
  string transfer_id = 1;
  int64  offset      = 2;
  bytes  data        = 3;   // 块数据 (最大 64KB/块)
}

message FileCompleteNotify {
  string transfer_id = 1;
  bool   success     = 2;
  string file_hash   = 3;   // 校验哈希
}
```

### 2.1 错误码定义

| 错误码 | 常量名 | 说明 |
|--------|--------|------|
| 0 | `OK` | 成功 |
| 1001 | `ERR_INVALID_PARAM` | 参数错误 |
| 2001 | `ERR_AUTH_FAILED` | 用户名或密码错误 |
| 2002 | `ERR_TOKEN_EXPIRED` | Token 已过期 |
| 2003 | `ERR_RATE_LIMITED` | 登录频率过高，已锁定 |
| 2004 | `ERR_ALREADY_ONLINE` | 已在其他设备登录 |
| 3001 | `ERR_MEETING_NOT_FOUND` | 会议不存在 |
| 3002 | `ERR_MEETING_FULL` | 会议人数已满 |
| 3003 | `ERR_MEETING_PASSWORD` | 会议密码错误 |
| 3004 | `ERR_NOT_HOST` | 非主持人无权操作 |
| 3005 | `ERR_ALREADY_IN_MEETING` | 已在会议中 |
| 4001 | `ERR_MSG_TOO_LONG` | 消息超过长度限制 |
| 5001 | `ERR_FILE_TOO_LARGE` | 文件超过大小限制 |
| 5002 | `ERR_TRANSFER_REJECTED` | 对方拒绝接收文件 |

---

## 3. 信令服务器 API（Go）

### 3.1 接口总表

| 方向 | SignalType | 请求消息 | 响应/通知消息 | 要求状态 |
|------|-----------|---------|-------------|---------|
| C→S | `AUTH_LOGIN_REQ` | `AuthLoginReq` | `AuthLoginRsp` | CONNECTED |
| C→S | `AUTH_LOGOUT_REQ` | `AuthLogoutReq` | `AuthLogoutRsp` | AUTHENTICATED+ |
| C↔S | `AUTH_HEARTBEAT_*` | `AuthHeartbeatReq` | `AuthHeartbeatRsp` | AUTHENTICATED+ |
| S→C | `AUTH_KICK_NOTIFY` | — | `AuthKickNotify` | — |
| C→S | `MEET_CREATE_REQ` | `MeetCreateReq` | `MeetCreateRsp` | AUTHENTICATED |
| C→S | `MEET_JOIN_REQ` | `MeetJoinReq` | `MeetJoinRsp` | AUTHENTICATED |
| C→S | `MEET_LEAVE_REQ` | `MeetLeaveReq` | `MeetLeaveRsp` | IN_MEETING |
| C→S | `MEET_KICK_REQ` | `MeetKickReq` | `MeetKickRsp` | IN_MEETING (Host) |
| C→S | `MEET_MUTE_ALL_REQ` | `MeetMuteAllReq` | `MeetMuteAllRsp` | IN_MEETING (Host) |
| S→C | `MEET_STATE_SYNC` | — | `MeetStateSyncNotify` | — |
| S→C | `MEET_PARTICIPANT_JOIN` | — | `MeetParticipantJoinNotify` | — |
| S→C | `MEET_PARTICIPANT_LEAVE` | — | `MeetParticipantLeaveNotify` | — |
| S→C | `MEET_HOST_CHANGED` | — | `MeetHostChangedNotify` | — |
| C→S | `MEDIA_OFFER` | `MediaOffer` | (转发) | IN_MEETING |
| C→S | `MEDIA_ANSWER` | `MediaAnswer` | (转发) | IN_MEETING |
| C→S | `MEDIA_ICE_CANDIDATE` | `MediaIceCandidate` | (转发) | IN_MEETING |
| C→S | `MEDIA_MUTE_TOGGLE` | `MediaMuteToggle` | (广播) | IN_MEETING |
| C→S | `MEDIA_SCREEN_SHARE` | `MediaScreenShare` | (广播) | IN_MEETING |
| C→S | `CHAT_SEND_REQ` | `ChatSendReq` | `ChatSendRsp` | IN_MEETING |
| S→C | `CHAT_RECV_NOTIFY` | — | `ChatRecvNotify` | — |
| C→S | `FILE_OFFER_REQ` | `FileOfferReq` | `FileOfferRsp` | IN_MEETING |
| C→S | `FILE_ACCEPT_REQ` | `FileAcceptReq` | — | IN_MEETING |
| C↔S | `FILE_CHUNK_DATA` | `FileChunkData` | — | IN_MEETING |
| S→C | `FILE_COMPLETE_NOTIFY` | — | `FileCompleteNotify` | — |

### 3.2 Go Handler 接口签名

```go
// === AuthHandler ===
type AuthHandler struct {
    userRepo   *store.UserRepo
    redis      *redis.Client
    jwtManager *auth.JWTManager
    limiter    *auth.RateLimiter
    sessions   *SessionManager
}

// Login 处理登录请求
//   1. RateLimiter 检查 IP 频率
//   2. MySQL 查询用户 → Argon2id 比对密码
//   3. 生成 JWT Token
//   4. Redis 写入 session:{user_id}
//   5. SessionManager.BindUser() (踢旧会话)
//   6. 回复 AuthLoginRsp
func (h *AuthHandler) HandleLogin(s *Session, req *pb.AuthLoginReq) error

// Logout 处理登出
//   1. Redis 删除 session:{user_id}
//   2. 如在会议中 → 触发离会逻辑
//   3. 回复 AuthLogoutRsp
func (h *AuthHandler) HandleLogout(s *Session, req *pb.AuthLogoutReq) error

// Heartbeat 心跳续期
//   1. Redis EXPIRE session:{user_id} 延长 TTL
//   2. 回复 AuthHeartbeatRsp (携带服务端时间戳)
func (h *AuthHandler) HandleHeartbeat(s *Session, req *pb.AuthHeartbeatReq) error
```

```go
// === MeetingHandler ===
type MeetingHandler struct {
    meetRepo   *store.MeetingRepo
    redis      *redis.Client
    sessions   *SessionManager
    sfuManager *SFUNodeManager
}

// Create 创建会议
//   1. 生成 6-8 位唯一会议号
//   2. MySQL 插入 meetings 记录
//   3. Redis 创建 room:{meeting_id}
//   4. 创建者自动加入（触发 Join 流程）
//   5. 回复 MeetCreateRsp
func (h *MeetingHandler) HandleCreate(s *Session, req *pb.MeetCreateReq) error

// Join 加入会议
//   1. Redis 查 room → 验证密码 + 人数上限
//   2. 如房间无 SFU → 分配最低负载 SFU 节点
//   3. Redis SADD room_members + HSET room_member
//   4. MySQL 插入 meeting_participants
//   5. 生成临时 TURN 凭证 (HMAC-SHA1)
//   6. 广播 MeetParticipantJoinNotify
//   7. 回复 MeetJoinRsp (含 SFU 地址 + ICE 列表 + 参会者)
func (h *MeetingHandler) HandleJoin(s *Session, req *pb.MeetJoinReq) error

// Leave 离开会议
//   1. Redis SREM + DEL member
//   2. MySQL UPDATE left_at
//   3. 广播 MeetParticipantLeaveNotify
//   4. 检查是否最后一人 → 销毁房间
//   5. 主持人离开 → 自动转移主持人
//   6. 回复 MeetLeaveRsp
func (h *MeetingHandler) HandleLeave(s *Session, req *pb.MeetLeaveReq) error

// Kick 踢人 (仅主持人)
//   1. 校验 caller 是否为 Host
//   2. 向目标用户发送 MeetParticipantLeaveNotify(reason="被踢出")
//   3. 强制目标 Session 离会
func (h *MeetingHandler) HandleKick(s *Session, req *pb.MeetKickReq) error
```

```go
// === ChatHandler ===
type ChatHandler struct {
    msgRepo  *store.MessageRepo
    sessions *SessionManager
}

// Send 发送聊天消息
//   1. 参数校验 (长度 ≤ 5000)
//   2. MySQL 插入 messages 记录
//   3. 房间广播 ChatRecvNotify (排除发送者)
//   4. 回复 ChatSendRsp (message_id + server_timestamp)
func (h *ChatHandler) HandleSend(s *Session, req *pb.ChatSendReq) error
```

```go
// === MediaNegotiationHandler ===
type MediaNegotiationHandler struct {
    sessions *SessionManager
}

// HandleOffer / HandleAnswer / HandleIceCandidate
// 纯转发：根据 target_user_id 查 Redis 定位 Session → 原样转发
// 不做任何解析或存储
func (h *MediaNegotiationHandler) HandleOffer(s *Session, req *pb.MediaOffer) error
func (h *MediaNegotiationHandler) HandleAnswer(s *Session, req *pb.MediaAnswer) error
func (h *MediaNegotiationHandler) HandleIce(s *Session, req *pb.MediaIceCandidate) error

// HandleMuteToggle / HandleScreenShare
// 更新 Redis room_member 状态 → 房间广播通知
func (h *MediaNegotiationHandler) HandleMuteToggle(s *Session, req *pb.MediaMuteToggle) error
func (h *MediaNegotiationHandler) HandleScreenShare(s *Session, req *pb.MediaScreenShare) error
```
