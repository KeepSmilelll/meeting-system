package handler

import (
    "meeting-server/signaling/auth"
    "meeting-server/signaling/protocol"
    "meeting-server/signaling/server"
    "meeting-server/signaling/store"
    "time"
)

type AuthHandler struct {
    sessions *server.SessionManager
    store    *store.MemoryStore
    tokens   *auth.TokenManager
}

func NewAuthHandler(sessions *server.SessionManager, memStore *store.MemoryStore, tokens *auth.TokenManager) *AuthHandler {
    return &AuthHandler{sessions: sessions, store: memStore, tokens: tokens}
}

func (h *AuthHandler) HandleLogin(session *server.Session, payload []byte) {
    var req protocol.AuthLoginReqBody
    if !decodeJSON(payload, &req) || req.Username == "" || req.PasswordHash == "" {
        _ = session.Send(protocol.AuthLoginRsp, protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
        return
    }

    user, err := h.store.Authenticate(req.Username, req.PasswordHash)
    if err != nil {
        _ = session.Send(protocol.AuthLoginRsp, protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrAuthFailed, Message: "invalid username or password"}})
        return
    }

    token, err := h.tokens.Generate(user.ID)
    if err != nil {
        _ = session.Send(protocol.AuthLoginRsp, protocol.AuthLoginRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "token generation failed"}})
        return
    }

    h.sessions.BindUser(session, user.ID)
    session.SetState(server.StateAuthenticated)
    session.SetMeetingID("")

    _ = session.Send(protocol.AuthLoginRsp, protocol.AuthLoginRspBody{
        Success:     true,
        Token:       token,
        UserID:      user.ID,
        DisplayName: user.DisplayName,
        AvatarURL:   user.AvatarURL,
    })
}

func (h *AuthHandler) HandleLogout(session *server.Session, payload []byte) {
    _ = payload
    _ = session.Send(protocol.AuthLogoutRsp, protocol.AuthLogoutRspBody{Success: true})
    session.Close()
}

func (h *AuthHandler) HandleHeartbeat(session *server.Session, payload []byte) {
    _ = payload
    _ = session.Send(protocol.AuthHeartbeatRsp, protocol.AuthHeartbeatRspBody{ServerTimestamp: time.Now().UnixMilli()})
}

