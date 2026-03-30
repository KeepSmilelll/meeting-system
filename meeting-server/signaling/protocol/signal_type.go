package protocol

type SignalType uint16

const (
	AuthLoginReq     SignalType = 0x0101
	AuthLoginRsp     SignalType = 0x0102
	AuthLogoutReq    SignalType = 0x0103
	AuthLogoutRsp    SignalType = 0x0104
	AuthHeartbeatReq SignalType = 0x0105
	AuthHeartbeatRsp SignalType = 0x0106
	AuthKickNotify   SignalType = 0x0107

	MeetCreateReq        SignalType = 0x0201
	MeetCreateRsp        SignalType = 0x0202
	MeetJoinReq          SignalType = 0x0203
	MeetJoinRsp          SignalType = 0x0204
	MeetLeaveReq         SignalType = 0x0205
	MeetLeaveRsp         SignalType = 0x0206
	MeetKickReq          SignalType = 0x0207
	MeetKickRsp          SignalType = 0x0208
	MeetMuteAllReq       SignalType = 0x0209
	MeetMuteAllRsp       SignalType = 0x020A
	MeetStateSync        SignalType = 0x020B
	MeetParticipantJoin  SignalType = 0x020C
	MeetParticipantLeave SignalType = 0x020D
	MeetHostChanged      SignalType = 0x020E

	MediaOffer        SignalType = 0x0301
	MediaAnswer       SignalType = 0x0302
	MediaIceCandidate SignalType = 0x0303
	MediaMuteToggle   SignalType = 0x0304
	MediaScreenShare  SignalType = 0x0305

	ChatSendReq    SignalType = 0x0401
	ChatSendRsp    SignalType = 0x0402
	ChatRecvNotify SignalType = 0x0403

	FileOfferReq       SignalType = 0x0501
	FileOfferRsp       SignalType = 0x0502
	FileAcceptReq      SignalType = 0x0503
	FileChunkData      SignalType = 0x0504
	FileCompleteNotify SignalType = 0x0505
)
