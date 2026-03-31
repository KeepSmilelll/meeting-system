package handler

import (
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
)

type MediaHandler struct {
	sessions *server.SessionManager
}

func NewMediaHandler(sessions *server.SessionManager) *MediaHandler {
	return &MediaHandler{sessions: sessions}
}

func (h *MediaHandler) HandleOffer(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaOffer)
}

func (h *MediaHandler) HandleAnswer(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaAnswer)
}

func (h *MediaHandler) HandleIceCandidate(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaIceCandidate)
}

func (h *MediaHandler) forward(session *server.Session, payload []byte, msgType protocol.SignalType) {
	if h == nil || h.sessions == nil || session == nil {
		return
	}
	if session.MeetingID() == "" || session.UserID() == "" {
		return
	}

	targetUserID := ""
	switch msgType {
	case protocol.MediaOffer:
		var req protocol.MediaOfferBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
	case protocol.MediaAnswer:
		var req protocol.MediaAnswerBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
	case protocol.MediaIceCandidate:
		var req protocol.MediaIceCandidateBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
	default:
		return
	}

	if targetUserID == session.UserID() {
		return
	}

	target, ok := h.sessions.GetByUser(targetUserID)
	if !ok || target == nil {
		return
	}
	if target.MeetingID() != session.MeetingID() || target.State() != server.StateInMeeting {
		return
	}

	_ = target.SendRaw(protocol.EncodeFrame(msgType, payload))
}
