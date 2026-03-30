package handler

import (
    "meeting-server/signaling/config"
    "meeting-server/signaling/protocol"
    "meeting-server/signaling/server"
    "meeting-server/signaling/store"
)

type ChatHandler struct {
    cfg      config.Config
    sessions *server.SessionManager
    store    *store.MemoryStore
}

func NewChatHandler(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore) *ChatHandler {
    return &ChatHandler{cfg: cfg, sessions: sessions, store: memStore}
}

func (h *ChatHandler) HandleSend(session *server.Session, payload []byte) {
    var req protocol.ChatSendReqBody
    if !decodeJSON(payload, &req) || req.Content == "" {
        _ = session.Send(protocol.ChatSendRsp, protocol.ChatSendRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
        return
    }

    if len(req.Content) > h.cfg.MaxMessageLen {
        _ = session.Send(protocol.ChatSendRsp, protocol.ChatSendRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrMessageTooLong, Message: "message too long"}})
        return
    }

    msg := h.store.SaveMessage(session.MeetingID(), session.UserID(), req.Type, req.Content, req.ReplyToID)
    sender, _ := h.store.GetUserByID(session.UserID())

    notify := protocol.ChatRecvNotifyBody{
        MessageID:  msg.ID,
        SenderID:   msg.SenderID,
        SenderName: sender.DisplayName,
        Type:       msg.Type,
        Content:    msg.Content,
        ReplyToID:  msg.ReplyToID,
        Timestamp:  msg.Timestamp,
    }

    h.sessions.BroadcastToMeeting(session.MeetingID(), protocol.ChatRecvNotify, notify, session.UserID())

    _ = session.Send(protocol.ChatSendRsp, protocol.ChatSendRspBody{Success: true, MessageID: msg.ID, Timestamp: msg.Timestamp})
}

