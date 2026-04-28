package handler

import (
	"context"
	"fmt"
	"meeting-server/signaling/config"
	"meeting-server/signaling/model"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"strconv"
	"strings"
)

type ChatHandler struct {
	cfg         config.Config
	sessions    *server.SessionManager
	store       *store.MemoryStore
	messageRepo *store.MessageRepo
}

func NewChatHandler(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore, messageRepo ...*store.MessageRepo) *ChatHandler {
	var repo *store.MessageRepo
	if len(messageRepo) > 0 {
		repo = messageRepo[0]
	}
	if repo == nil {
		repo = store.NewInMemoryMessageRepo()
	}
	return &ChatHandler{cfg: cfg, sessions: sessions, store: memStore, messageRepo: repo}
}

func (h *ChatHandler) HandleSend(session *server.Session, payload []byte) {
	var req protocol.ChatSendReqBody
	if !decodeProto(payload, &req) || strings.TrimSpace(req.Content) == "" || strings.TrimSpace(session.MeetingID()) == "" {
		_ = session.Send(protocol.ChatSendRsp, &protocol.ChatSendRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	if len(req.Content) > h.cfg.MaxMessageLen {
		_ = session.Send(protocol.ChatSendRsp, &protocol.ChatSendRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrMessageTooLong, Message: "message too long"}})
		return
	}

	notify, err := h.saveMessage(session, &req)
	if err != nil {
		_ = session.Send(protocol.ChatSendRsp, &protocol.ChatSendRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInternal, Message: "save message failed"}})
		return
	}

	h.sessions.BroadcastToMeeting(session.MeetingID(), protocol.ChatRecvNotify, &notify, session.UserID())

	_ = session.Send(protocol.ChatSendRsp, &protocol.ChatSendRspBody{Success: true, MessageId: notify.MessageId, Timestamp: notify.Timestamp})
}

func (h *ChatHandler) HandleHistory(session *server.Session, payload []byte) {
	var req protocol.ChatHistoryReqBody
	if !decodeProto(payload, &req) {
		_ = session.Send(protocol.ChatHistoryRsp, &protocol.ChatHistoryRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	meetingID := strings.TrimSpace(req.MeetingId)
	if meetingID == "" {
		meetingID = strings.TrimSpace(session.MeetingID())
	}
	if meetingID == "" || meetingID != strings.TrimSpace(session.MeetingID()) {
		_ = session.Send(protocol.ChatHistoryRsp, &protocol.ChatHistoryRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "meeting mismatch"}})
		return
	}

	limit := int(req.Limit)
	if limit <= 0 || limit > 50 {
		limit = 50
	}

	messages, err := h.listMessages(meetingID, limit)
	if err != nil {
		_ = session.Send(protocol.ChatHistoryRsp, &protocol.ChatHistoryRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInternal, Message: "load history failed"}})
		return
	}

	rsp := &protocol.ChatHistoryRspBody{Success: true}
	for _, message := range messages {
		item := message
		rsp.Messages = append(rsp.Messages, &item)
	}
	_ = session.Send(protocol.ChatHistoryRsp, rsp)
}

func (h *ChatHandler) saveMessage(session *server.Session, req *protocol.ChatSendReqBody) (protocol.ChatRecvNotifyBody, error) {
	meetingID := strings.TrimSpace(session.MeetingID())
	userID := strings.TrimSpace(session.UserID())
	senderName := h.senderDisplayName(userID)

	numericMeetingID, meetingOK := parseNumericID(meetingID)
	numericSenderID, senderOK := parseNumericID(userID)
	if h.messageRepo != nil && meetingOK && senderOK {
		var replyTo *uint64
		if parsedReplyTo, ok := parseNumericID(req.ReplyToId); ok {
			replyTo = &parsedReplyTo
		}

		message := &model.Message{
			MeetingID: numericMeetingID,
			SenderID:  numericSenderID,
			Type:      int(req.Type),
			Content:   req.Content,
			ReplyToID: replyTo,
		}
		if err := h.messageRepo.Save(context.Background(), message); err != nil {
			return protocol.ChatRecvNotifyBody{}, err
		}
		return protocol.ChatRecvNotifyBody{
			MessageId:  strconv.FormatUint(message.ID, 10),
			SenderId:   userID,
			SenderName: senderName,
			Type:       req.Type,
			Content:    message.Content,
			ReplyToId:  req.ReplyToId,
			Timestamp:  message.CreatedAt.UnixMilli(),
		}, nil
	}

	if h.store == nil {
		return protocol.ChatRecvNotifyBody{}, fmt.Errorf("chat handler: no message store")
	}
	msg := h.store.SaveMessage(meetingID, userID, req.Type, req.Content, req.ReplyToId)
	return protocol.ChatRecvNotifyBody{
		MessageId:  msg.ID,
		SenderId:   msg.SenderID,
		SenderName: senderName,
		Type:       msg.Type,
		Content:    msg.Content,
		ReplyToId:  msg.ReplyToID,
		Timestamp:  msg.Timestamp,
	}, nil
}

func (h *ChatHandler) listMessages(meetingID string, limit int) ([]protocol.ChatRecvNotifyBody, error) {
	numericMeetingID, ok := parseNumericID(meetingID)
	if h.messageRepo != nil && ok {
		messages, err := h.messageRepo.ListByMeeting(context.Background(), numericMeetingID, limit)
		if err != nil {
			return nil, err
		}
		out := make([]protocol.ChatRecvNotifyBody, 0, len(messages))
		for _, msg := range messages {
			senderID := formatUserID(msg.SenderID)
			replyToID := ""
			if msg.ReplyToID != nil {
				replyToID = strconv.FormatUint(*msg.ReplyToID, 10)
			}
			out = append(out, protocol.ChatRecvNotifyBody{
				MessageId:  strconv.FormatUint(msg.ID, 10),
				SenderId:   senderID,
				SenderName: h.senderDisplayName(senderID),
				Type:       int32(msg.Type),
				Content:    msg.Content,
				ReplyToId:  replyToID,
				Timestamp:  msg.CreatedAt.UnixMilli(),
			})
		}
		return out, nil
	}

	if h.store == nil {
		return nil, fmt.Errorf("chat handler: no message store")
	}
	messages := h.store.ListMessages(meetingID, limit)
	out := make([]protocol.ChatRecvNotifyBody, 0, len(messages))
	for _, msg := range messages {
		out = append(out, protocol.ChatRecvNotifyBody{
			MessageId:  msg.ID,
			SenderId:   msg.SenderID,
			SenderName: h.senderDisplayName(msg.SenderID),
			Type:       msg.Type,
			Content:    msg.Content,
			ReplyToId:  msg.ReplyToID,
			Timestamp:  msg.Timestamp,
		})
	}
	return out, nil
}

func (h *ChatHandler) senderDisplayName(userID string) string {
	if h.store == nil {
		return userID
	}
	if sender, ok := h.store.GetUserByID(userID); ok && strings.TrimSpace(sender.DisplayName) != "" {
		return sender.DisplayName
	}
	return userID
}

func parseNumericID(raw string) (uint64, bool) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return 0, false
	}
	if len(trimmed) > 1 && (trimmed[0] == 'u' || trimmed[0] == 'U') {
		trimmed = trimmed[1:]
	}
	value, err := strconv.ParseUint(trimmed, 10, 64)
	return value, err == nil && value > 0
}

func formatUserID(id uint64) string {
	if id == 0 {
		return ""
	}
	return "u" + strconv.FormatUint(id, 10)
}
