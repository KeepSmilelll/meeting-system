package handler

import (
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

type HandlerFunc func(session *server.Session, payload []byte)

type Router struct {
	handlers map[protocol.SignalType]HandlerFunc

	authHandler    *AuthHandler
	meetingHandler *MeetingHandler
	mediaHandler   *MediaHandler
	chatHandler    *ChatHandler
}

func NewRouter(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore, meetingStore store.MeetingLifecycleStore, roomStateStore *store.RedisRoomStore, tokenManager *auth.TokenManager, limiter *auth.RateLimiter, meetingMirror ...MeetingMirror) *Router {
	r := &Router{handlers: make(map[protocol.SignalType]HandlerFunc)}

	r.authHandler = NewAuthHandler(sessions, memStore, tokenManager, limiter)
	if meetingStore == nil {
		meetingStore = memStore
	}
	r.meetingHandler = NewMeetingHandler(cfg, sessions, meetingStore, roomStateStore, meetingMirror...)
	r.mediaHandler = NewMediaHandler(sessions)
	r.chatHandler = NewChatHandler(cfg, sessions, memStore)

	r.Register(protocol.AuthLoginReq, r.authHandler.HandleLogin)
	r.Register(protocol.AuthLogoutReq, r.authHandler.HandleLogout)
	r.Register(protocol.AuthHeartbeatReq, r.authHandler.HandleHeartbeat)

	r.Register(protocol.MeetCreateReq, r.meetingHandler.HandleCreate)
	r.Register(protocol.MeetJoinReq, r.meetingHandler.HandleJoin)
	r.Register(protocol.MeetLeaveReq, r.meetingHandler.HandleLeave)
	r.Register(protocol.MeetKickReq, r.meetingHandler.HandleKick)
	r.Register(protocol.MeetMuteAllReq, r.meetingHandler.HandleMuteAll)

	r.Register(protocol.MediaOffer, r.mediaHandler.HandleOffer)
	r.Register(protocol.MediaAnswer, r.mediaHandler.HandleAnswer)
	r.Register(protocol.MediaIceCandidate, r.mediaHandler.HandleIceCandidate)

	r.Register(protocol.ChatSendReq, r.chatHandler.HandleSend)

	return r
}

func (r *Router) Register(msgType protocol.SignalType, fn HandlerFunc) {
	r.handlers[msgType] = fn
}

func (r *Router) HandleMessage(session *server.Session, msgType protocol.SignalType, payload []byte) {
	if !isStateAllowed(session.State(), msgType) {
		_ = session.Send(protocol.AuthKickNotify, &protocol.AuthKickNotifyBody{Reason: "invalid session state"})
		session.Close()
		return
	}

	fn, ok := r.handlers[msgType]
	if !ok {
		return
	}

	fn(session, payload)
}

func (r *Router) OnSessionClosed(session *server.Session) {
	if session.MeetingID() != "" {
		r.meetingHandler.LeaveByDisconnect(session)
	}
}

func decodeProto(payload []byte, out proto.Message) bool {
	if len(payload) == 0 {
		return false
	}
	return proto.Unmarshal(payload, out) == nil
}

func isStateAllowed(state server.SessionState, msgType protocol.SignalType) bool {
	switch msgType {
	case protocol.AuthLoginReq:
		return state == server.StateConnected || state == server.StateAuthenticated
	case protocol.AuthLogoutReq, protocol.AuthHeartbeatReq:
		return state == server.StateAuthenticated || state == server.StateInMeeting
	case protocol.MeetCreateReq, protocol.MeetJoinReq:
		return state == server.StateAuthenticated
	case protocol.MeetLeaveReq, protocol.ChatSendReq, protocol.MeetKickReq, protocol.MeetMuteAllReq:
		return state == server.StateInMeeting
	case protocol.MediaOffer, protocol.MediaAnswer, protocol.MediaIceCandidate:
		return state == server.StateInMeeting
	default:
		return true
	}
}
