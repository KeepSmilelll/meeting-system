package handler

import (
	"context"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

type HandlerFunc func(session *server.Session, payload []byte)

type Router struct {
	cfg      config.Config
	handlers map[protocol.SignalType]HandlerFunc

	authHandler    *AuthHandler
	meetingHandler *MeetingHandler
	mediaHandler   *MediaHandler
	chatHandler    *ChatHandler
	fileHandler    *FileHandler
	sessions       *server.SessionManager
	sessionStore   store.SessionStore
}

func NewRouter(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore, meetingStore store.MeetingLifecycleStore, roomStateStore *store.RedisRoomStore, sessionStore store.SessionStore, tokenManager *auth.TokenManager, limiter *auth.RateLimiter, mediaSfuClient signalingSfu.Client, directBus store.UserEventPublisher, meetingMirror ...MeetingMirror) *Router {
	r := &Router{cfg: cfg, handlers: make(map[protocol.SignalType]HandlerFunc), sessions: sessions, sessionStore: sessionStore}

	r.authHandler = NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, sessionStore, directBus)
	if meetingStore == nil {
		meetingStore = memStore
	}
	r.meetingHandler = NewMeetingHandler(cfg, sessions, meetingStore, roomStateStore, sessionStore, directBus, mediaSfuClient, meetingMirror...)
	r.mediaHandler = NewMediaHandler(cfg, sessions, meetingStore, roomStateStore, mediaSfuClient, sessionStore, directBus)
	r.chatHandler = NewChatHandler(cfg, sessions, memStore)
	r.fileHandler = NewFileHandler(cfg, sessions)

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
	r.Register(protocol.MediaMuteToggle, r.mediaHandler.HandleMuteToggle)
	r.Register(protocol.MediaScreenShare, r.mediaHandler.HandleScreenShare)

	r.Register(protocol.ChatSendReq, r.chatHandler.HandleSend)

	r.Register(protocol.FileOfferReq, r.fileHandler.HandleOffer)
	r.Register(protocol.FileAcceptReq, r.fileHandler.HandleAccept)
	r.Register(protocol.FileChunkData, r.fileHandler.HandleChunk)

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
		if session.LogoutClose() {
			r.meetingHandler.leave(session, false, "主动离开")
		} else {
			r.meetingHandler.LeaveByDisconnect(session)
		}
	}
	r.authHandler.OnSessionClosed(session)
}

func (r *Router) OnUserNodeEvent(event store.UserNodeEvent) {
	if r == nil || r.sessions == nil || event.TargetUserID == "" || event.TargetSessionID == 0 {
		return
	}

	target, ok := r.sessions.GetByUser(event.TargetUserID)
	if !ok || target == nil || target.ID != event.TargetSessionID {
		return
	}

	if event.ResetMeeting {
		target.SetMeetingID("")
	}
	if event.State != nil {
		target.SetState(server.SessionState(*event.State))
	}
	if event.ResetMeeting || event.State != nil {
		syncSessionPresence(context.Background(), r.cfg, r.sessionStore, target)
	}
	if len(event.Frame) > 0 {
		_ = target.SendRaw(event.Frame)
	}
	if event.Close {
		target.Close()
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
	case protocol.MeetLeaveReq, protocol.ChatSendReq, protocol.MeetKickReq, protocol.MeetMuteAllReq,
		protocol.FileOfferReq, protocol.FileAcceptReq, protocol.FileChunkData:
		return state == server.StateInMeeting
	case protocol.MediaOffer, protocol.MediaAnswer, protocol.MediaIceCandidate, protocol.MediaMuteToggle, protocol.MediaScreenShare:
		return state == server.StateInMeeting
	default:
		return true
	}
}
