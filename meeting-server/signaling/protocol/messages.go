package protocol

import "meeting-server/signaling/protocol/pb"

const (
	OK                  int32 = 0
	ErrInvalidParam     int32 = 1001
	ErrInternal         int32 = 1002
	ErrAuthFailed       int32 = 2001
	ErrTokenExpired     int32 = 2002
	ErrRateLimited      int32 = 2003
	ErrAlreadyOnline    int32 = 2004
	ErrMeetingNotFound  int32 = 3001
	ErrMeetingFull      int32 = 3002
	ErrMeetingPassword  int32 = 3003
	ErrNotHost          int32 = 3004
	ErrAlreadyInMeeting int32 = 3005
	ErrMessageTooLong   int32 = 4001
	ErrFileTooLarge     int32 = 5001
	ErrTransferRejected int32 = 5002
)

type (
	AuthLoginReqBody               = pb.AuthLoginReq
	AuthLoginRspBody               = pb.AuthLoginRsp
	AuthLogoutReqBody              = pb.AuthLogoutReq
	AuthLogoutRspBody              = pb.AuthLogoutRsp
	AuthHeartbeatReqBody           = pb.AuthHeartbeatReq
	AuthHeartbeatRspBody           = pb.AuthHeartbeatRsp
	AuthKickNotifyBody             = pb.AuthKickNotify
	MeetCreateReqBody              = pb.MeetCreateReq
	MeetCreateRspBody              = pb.MeetCreateRsp
	MeetJoinReqBody                = pb.MeetJoinReq
	MeetJoinRspBody                = pb.MeetJoinRsp
	MeetLeaveReqBody               = pb.MeetLeaveReq
	MeetLeaveRspBody               = pb.MeetLeaveRsp
	MeetKickReqBody                = pb.MeetKickReq
	MeetKickRspBody                = pb.MeetKickRsp
	MeetMuteAllReqBody             = pb.MeetMuteAllReq
	MeetMuteAllRspBody             = pb.MeetMuteAllRsp
	MeetStateSyncNotifyBody        = pb.MeetStateSyncNotify
	MeetParticipantJoinNotifyBody  = pb.MeetParticipantJoinNotify
	MeetParticipantLeaveNotifyBody = pb.MeetParticipantLeaveNotify
	MeetHostChangedNotifyBody      = pb.MeetHostChangedNotify
	MediaOfferBody                 = pb.MediaOffer
	MediaAnswerBody                = pb.MediaAnswer
	MediaIceCandidateBody          = pb.MediaIceCandidate
	MediaMuteToggleBody            = pb.MediaMuteToggle
	MediaScreenShareBody           = pb.MediaScreenShare
	MediaRouteStatusNotifyBody     = pb.AuthKickNotify
	ChatSendReqBody                = pb.ChatSendReq
	ChatSendRspBody                = pb.ChatSendRsp
	ChatRecvNotifyBody             = pb.ChatRecvNotify
	ChatHistoryReqBody             = pb.ChatHistoryReq
	ChatHistoryRspBody             = pb.ChatHistoryRsp
	FileOfferReqBody               = pb.FileOfferReq
	FileOfferRspBody               = pb.FileOfferRsp
	FileAcceptReqBody              = pb.FileAcceptReq
	FileChunkDataBody              = pb.FileChunkData
	FileCompleteNotifyBody         = pb.FileCompleteNotify
	IceServer                      = pb.IceServer
	Participant                    = pb.Participant
	ErrorInfo                      = pb.ErrorInfo
)
