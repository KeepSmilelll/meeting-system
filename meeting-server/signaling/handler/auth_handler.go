package handler

import (
	"context"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"time"
)

type AuthHandler struct {
	cfg          config.Config
	sessions     *server.SessionManager
	store        *store.MemoryStore
	tokens       *auth.TokenManager
	limiter      *auth.RateLimiter
	sessionStore store.SessionStore
	nodeBus      store.UserEventPublisher
}

func NewAuthHandler(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore, tokens *auth.TokenManager, limiter *auth.RateLimiter, sessionStore store.SessionStore, nodeBus store.UserEventPublisher) *AuthHandler {
	return &AuthHandler{cfg: cfg, sessions: sessions, store: memStore, tokens: tokens, limiter: limiter, sessionStore: sessionStore, nodeBus: nodeBus}
}

func (h *AuthHandler) HandleLogin(session *server.Session, payload []byte) {
	var req protocol.AuthLoginReqBody
	if !decodeProto(payload, &req) || req.Username == "" || (req.PasswordHash == "" && req.ResumeToken == "") {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	allowed, _, err := h.limiter.Allow(context.Background(), "login:"+req.Username)
	if err == nil && !allowed {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrRateLimited, Message: "too many login attempts"}})
		return
	}

	user, err := h.authenticate(req.Username, req.PasswordHash, req.ResumeToken)
	if err != nil {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrAuthFailed, Message: "invalid username or password"}})
		return
	}

	token, err := h.tokens.Generate(user.ID)
	if err != nil {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "token generation failed"}})
		return
	}

	ctx := context.Background()
	var existingPresence *store.SessionPresence
	if h.sessionStore != nil {
		existingPresence, _ = h.sessionStore.Get(ctx, user.ID)
	}
	h.sessions.BindUser(session, user.ID)
	session.SetState(server.StateAuthenticated)
	session.SetMeetingID("")
	syncSessionPresence(ctx, h.cfg, h.sessionStore, session)
	h.kickRemoteExistingSession(ctx, existingPresence, session.ID)

	_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{
		Success:     true,
		Token:       token,
		UserId:      user.ID,
		DisplayName: user.DisplayName,
		AvatarUrl:   user.AvatarURL,
	})
}

func (h *AuthHandler) authenticate(username, passwordHash, resumeToken string) (store.User, error) {
	if resumeToken != "" {
		return h.authenticateWithToken(username, resumeToken)
	}

	user, err := h.store.Authenticate(username, passwordHash)
	if err == nil {
		return user, nil
	}

	// Temporary compatibility path: older clients still put the cached token in password_hash.
	userID, verifyErr := h.tokens.Verify(passwordHash)
	if verifyErr != nil {
		return store.User{}, err
	}

	user, ok := h.store.GetUserByID(userID)
	if !ok || user.Username != username {
		return store.User{}, err
	}

	return user, nil
}

func (h *AuthHandler) authenticateWithToken(username, token string) (store.User, error) {
	userID, err := h.tokens.Verify(token)
	if err != nil {
		return store.User{}, err
	}

	user, ok := h.store.GetUserByID(userID)
	if !ok || user.Username != username {
		return store.User{}, store.ErrInvalidPassword
	}

	return user, nil
}

func (h *AuthHandler) HandleLogout(session *server.Session, payload []byte) {
	_ = payload
	if session.MeetingID() != "" {
		session.MarkLogoutClose()
	}
	clearSessionPresence(context.Background(), h.sessionStore, session)
	_ = session.Send(protocol.AuthLogoutRsp, &protocol.AuthLogoutRspBody{Success: true})
	session.Close()
}

func (h *AuthHandler) HandleHeartbeat(session *server.Session, payload []byte) {
	_ = payload
	syncSessionPresence(context.Background(), h.cfg, h.sessionStore, session)
	_ = session.Send(protocol.AuthHeartbeatRsp, &protocol.AuthHeartbeatRspBody{ServerTimestamp: time.Now().UnixMilli()})
}

func (h *AuthHandler) OnSessionClosed(session *server.Session) {
	clearSessionPresence(context.Background(), h.sessionStore, session)
}

func (h *AuthHandler) kickRemoteExistingSession(ctx context.Context, existing *store.SessionPresence, newSessionID uint64) {
	if h == nil || h.nodeBus == nil || existing == nil {
		return
	}
	if existing.NodeID == "" || existing.NodeID == h.cfg.NodeID || existing.SessionID == 0 || existing.SessionID == newSessionID {
		return
	}

	_ = h.nodeBus.PublishUserControl(ctx, store.UserNodeEvent{
		TargetNodeID:    existing.NodeID,
		TargetUserID:    existing.UserID,
		TargetSessionID: existing.SessionID,
		Frame:           protocol.EncodeFrame(protocol.AuthKickNotify, mustMarshalProto(&protocol.AuthKickNotifyBody{Reason: "账号在其他设备登录"})),
		Close:           true,
	})
}
