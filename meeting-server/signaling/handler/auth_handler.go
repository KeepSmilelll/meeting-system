package handler

import (
	"context"
	"log"
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
	startedAt := time.Now()
	var req protocol.AuthLoginReqBody
	if !decodeProto(payload, &req) || req.Username == "" || (req.PasswordHash == "" && req.ResumeToken == "") {
		log.Printf("auth login invalid session=%d payload_len=%d", session.ID, len(payload))
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	log.Printf("auth login begin session=%d username=%q", session.ID, req.Username)

	limiterStartedAt := time.Now()
	limiterCtx, limiterCancel := context.WithTimeout(context.Background(), h.loginDependencyTimeout())
	allowed, _, err := h.limiter.Allow(limiterCtx, "login:"+req.Username)
	limiterCancel()
	log.Printf("auth login limiter session=%d username=%q allowed=%t err=%v duration=%s", session.ID, req.Username, allowed, err, time.Since(limiterStartedAt))
	if err == nil && !allowed {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrRateLimited, Message: "too many login attempts"}})
		return
	}

	authStartedAt := time.Now()
	user, err := h.authenticate(req.Username, req.PasswordHash, req.ResumeToken)
	log.Printf("auth login authenticate session=%d username=%q user_id=%q err=%v duration=%s", session.ID, req.Username, user.ID, err, time.Since(authStartedAt))
	if err != nil {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrAuthFailed, Message: "invalid username or password"}})
		return
	}

	tokenStartedAt := time.Now()
	token, err := h.tokens.Generate(user.ID)
	log.Printf("auth login token session=%d user_id=%q err=%v duration=%s", session.ID, user.ID, err, time.Since(tokenStartedAt))
	if err != nil {
		_ = session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "token generation failed"}})
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), h.loginDependencyTimeout())
	defer cancel()
	var existingPresence *store.SessionPresence
	if h.sessionStore != nil {
		storeStartedAt := time.Now()
		var storeErr error
		existingPresence, storeErr = h.sessionStore.Get(ctx, user.ID)
		log.Printf("auth login session_store_get session=%d user_id=%q found=%t err=%v duration=%s", session.ID, user.ID, existingPresence != nil, storeErr, time.Since(storeStartedAt))
	}
	h.sessions.BindUser(session, user.ID)
	session.SetState(server.StateAuthenticated)
	session.SetMeetingID("")
	syncSessionPresence(ctx, h.cfg, h.sessionStore, session)
	h.kickRemoteExistingSession(ctx, existingPresence, session.ID)

	sendErr := session.Send(protocol.AuthLoginRsp, &protocol.AuthLoginRspBody{
		Success:     true,
		Token:       token,
		UserId:      user.ID,
		DisplayName: user.DisplayName,
		AvatarUrl:   user.AvatarURL,
	})
	log.Printf("auth login complete session=%d user_id=%q send_err=%v total=%s", session.ID, user.ID, sendErr, time.Since(startedAt))
}

func (h *AuthHandler) loginDependencyTimeout() time.Duration {
	timeout := h.cfg.AuthTimeout / 2
	if timeout <= 0 || timeout > 3*time.Second {
		return 3 * time.Second
	}
	if timeout < time.Second {
		return time.Second
	}
	return timeout
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
