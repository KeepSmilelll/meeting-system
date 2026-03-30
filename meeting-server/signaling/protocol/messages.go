package protocol

type ErrorInfo struct {
    Code    int32  `json:"code"`
    Message string `json:"message"`
}

const (
    OK                    int32 = 0
    ErrInvalidParam       int32 = 1001
    ErrAuthFailed         int32 = 2001
    ErrTokenExpired       int32 = 2002
    ErrRateLimited        int32 = 2003
    ErrAlreadyOnline      int32 = 2004
    ErrMeetingNotFound    int32 = 3001
    ErrMeetingFull        int32 = 3002
    ErrMeetingPassword    int32 = 3003
    ErrNotHost            int32 = 3004
    ErrAlreadyInMeeting   int32 = 3005
    ErrMessageTooLong     int32 = 4001
    ErrFileTooLarge       int32 = 5001
    ErrTransferRejected   int32 = 5002
)

type AuthLoginReqBody struct {
    Username     string `json:"username"`
    PasswordHash string `json:"password_hash"`
    DeviceID     string `json:"device_id"`
    Platform     string `json:"platform"`
}

type AuthLoginRspBody struct {
    Success     bool       `json:"success"`
    Error       *ErrorInfo `json:"error,omitempty"`
    Token       string     `json:"token,omitempty"`
    UserID      string     `json:"user_id,omitempty"`
    DisplayName string     `json:"display_name,omitempty"`
    AvatarURL   string     `json:"avatar_url,omitempty"`
}

type AuthLogoutRspBody struct {
    Success bool `json:"success"`
}

type AuthHeartbeatReqBody struct {
    ClientTimestamp int64 `json:"client_timestamp"`
}

type AuthHeartbeatRspBody struct {
    ServerTimestamp int64 `json:"server_timestamp"`
}

type AuthKickNotifyBody struct {
    Reason string `json:"reason"`
}

type MeetCreateReqBody struct {
    Title           string `json:"title"`
    Password        string `json:"password"`
    MaxParticipants int32  `json:"max_participants"`
}

type MeetCreateRspBody struct {
    Success   bool       `json:"success"`
    Error     *ErrorInfo `json:"error,omitempty"`
    MeetingID string     `json:"meeting_id,omitempty"`
}

type MeetJoinReqBody struct {
    MeetingID string `json:"meeting_id"`
    Password  string `json:"password"`
}

type IceServer struct {
    URLs       string `json:"urls"`
    Username   string `json:"username,omitempty"`
    Credential string `json:"credential,omitempty"`
}

type Participant struct {
    UserID      string `json:"user_id"`
    DisplayName string `json:"display_name"`
    AvatarURL   string `json:"avatar_url,omitempty"`
    Role        int32  `json:"role"`
    IsAudioOn   bool   `json:"is_audio_on"`
    IsVideoOn   bool   `json:"is_video_on"`
    IsSharing   bool   `json:"is_sharing"`
}

type MeetJoinRspBody struct {
    Success      bool          `json:"success"`
    Error        *ErrorInfo    `json:"error,omitempty"`
    MeetingID    string        `json:"meeting_id,omitempty"`
    Title        string        `json:"title,omitempty"`
    SFUAddress   string        `json:"sfu_address,omitempty"`
    IceServers   []IceServer   `json:"ice_servers,omitempty"`
    Participants []Participant `json:"participants,omitempty"`
}

type MeetLeaveRspBody struct {
    Success bool `json:"success"`
}

type MeetParticipantJoinNotifyBody struct {
    Participant Participant `json:"participant"`
}

type MeetParticipantLeaveNotifyBody struct {
    UserID string `json:"user_id"`
    Reason string `json:"reason"`
}

type ChatSendReqBody struct {
    Type      int32  `json:"type"`
    Content   string `json:"content"`
    ReplyToID string `json:"reply_to_id,omitempty"`
}

type ChatSendRspBody struct {
    Success   bool       `json:"success"`
    Error     *ErrorInfo `json:"error,omitempty"`
    MessageID string     `json:"message_id,omitempty"`
    Timestamp int64      `json:"timestamp,omitempty"`
}

type ChatRecvNotifyBody struct {
    MessageID  string `json:"message_id"`
    SenderID   string `json:"sender_id"`
    SenderName string `json:"sender_name"`
    Type       int32  `json:"type"`
    Content    string `json:"content"`
    ReplyToID  string `json:"reply_to_id,omitempty"`
    Timestamp  int64  `json:"timestamp"`
}

type MeetHostChangedNotifyBody struct {
    NewHostID   string `json:"new_host_id"`
    NewHostName string `json:"new_host_name"`
}

