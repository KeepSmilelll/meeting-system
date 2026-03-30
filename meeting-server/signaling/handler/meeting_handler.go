package handler

import (
	"context"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

type MeetingHandler struct {
	cfg       config.Config
	sessions  *server.SessionManager
	store     *store.MemoryStore
	roomState *store.RedisRoomStore
}

func NewMeetingHandler(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore, roomState *store.RedisRoomStore) *MeetingHandler {
	return &MeetingHandler{cfg: cfg, sessions: sessions, store: memStore, roomState: roomState}
}

func (h *MeetingHandler) HandleCreate(session *server.Session, payload []byte) {
	var req protocol.MeetCreateReqBody
	if !decodeProto(payload, &req) || req.Title == "" {
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	meeting, hostParticipant, err := h.store.CreateMeeting(req.Title, req.Password, session.UserID(), int(req.MaxParticipants))
	if err != nil {
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: err.Error()}})
		return
	}

	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)

	ctx := context.Background()
	_ = h.roomState.UpsertRoom(ctx, meeting.ID, hostParticipant.UserId)
	_ = h.roomState.AddMember(ctx, meeting.ID, hostParticipant)

	_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{Success: true, MeetingId: meeting.ID})

	_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{
		Success:      true,
		MeetingId:    meeting.ID,
		Title:        meeting.Title,
		SfuAddress:   h.cfg.DefaultSFUAddress,
		Participants: []*protocol.Participant{cloneParticipant(hostParticipant)},
	})
}

func (h *MeetingHandler) HandleJoin(session *server.Session, payload []byte) {
	var req protocol.MeetJoinReqBody
	if !decodeProto(payload, &req) || req.MeetingId == "" {
		_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	meeting, participants, joined, err := h.store.JoinMeeting(req.MeetingId, req.Password, session.UserID())
	if err != nil {
		code := protocol.ErrMeetingNotFound
		switch err {
		case store.ErrMeetingFull:
			code = protocol.ErrMeetingFull
		case store.ErrMeetingPassFailed:
			code = protocol.ErrMeetingPassword
		}

		_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{Success: false, Error: &protocol.ErrorInfo{Code: code, Message: err.Error()}})
		return
	}

	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)

	_ = h.roomState.AddMember(context.Background(), meeting.ID, joined)

	_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{
		Success:      true,
		MeetingId:    meeting.ID,
		Title:        meeting.Title,
		SfuAddress:   h.cfg.DefaultSFUAddress,
		Participants: cloneParticipants(participants),
	})

	h.sessions.BroadcastToRoom(
		meeting.ID,
		protocol.MeetParticipantJoin,
		&protocol.MeetParticipantJoinNotifyBody{Participant: cloneParticipant(joined)},
		session.UserID(),
	)
}

func (h *MeetingHandler) HandleLeave(session *server.Session, payload []byte) {
	_ = payload
	h.leave(session, true, "主动离开")
}

func (h *MeetingHandler) HandleKick(session *server.Session, payload []byte) {
	var req protocol.MeetKickReqBody
	if !decodeProto(payload, &req) || req.TargetUserId == "" {
		h.replyKickError(session, protocol.ErrInvalidParam, "bad request")
		return
	}

	meetingID := session.MeetingID()
	if meetingID == "" {
		h.replyKickError(session, protocol.ErrMeetingNotFound, "meeting not found")
		return
	}
	if req.TargetUserId == session.UserID() {
		h.replyKickError(session, protocol.ErrInvalidParam, "cannot kick yourself")
		return
	}

	isHost, err := h.store.IsMeetingHost(meetingID, session.UserID())
	if err != nil {
		h.replyKickError(session, protocol.ErrMeetingNotFound, err.Error())
		return
	}
	if !isHost {
		h.replyKickError(session, protocol.ErrNotHost, "only host can kick")
		return
	}

	exists, err := h.store.HasParticipant(meetingID, req.TargetUserId)
	if err != nil {
		h.replyKickError(session, protocol.ErrMeetingNotFound, err.Error())
		return
	}
	if !exists {
		h.replyKickError(session, protocol.ErrMeetingNotFound, "target not in meeting")
		return
	}

	hostChanged, newHost, remaining, err := h.store.LeaveMeeting(meetingID, req.TargetUserId)
	if err != nil {
		h.replyKickError(session, protocol.ErrMeetingNotFound, err.Error())
		return
	}

	_ = h.roomState.RemoveMember(context.Background(), meetingID, req.TargetUserId)

	reason := "被主持人移出会议"
	if kickedSession, ok := h.sessions.GetByUser(req.TargetUserId); ok && kickedSession.MeetingID() == meetingID {
		kickedSession.SetMeetingID("")
		kickedSession.SetState(server.StateAuthenticated)
		_ = kickedSession.Send(protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: req.TargetUserId, Reason: reason})
	}

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: req.TargetUserId, Reason: reason}, req.TargetUserId)
	if hostChanged && newHost != nil {
		_ = h.roomState.TransferHost(context.Background(), meetingID, newHost.UserId)
		h.sessions.BroadcastToRoom(meetingID, protocol.MeetHostChanged, &protocol.MeetHostChangedNotifyBody{NewHostId: newHost.UserId, NewHostName: newHost.DisplayName}, "")
	}
	if remaining == 0 {
		_ = h.roomState.DeleteRoom(context.Background(), meetingID)
	}

	_ = session.Send(protocol.MeetKickRsp, &protocol.MeetKickRspBody{Success: true})
}

func (h *MeetingHandler) HandleMuteAll(session *server.Session, payload []byte) {
	var req protocol.MeetMuteAllReqBody
	if !decodeProto(payload, &req) {
		_ = session.Send(protocol.MeetMuteAllRsp, &protocol.MeetMuteAllRspBody{Success: false})
		return
	}

	meetingID := session.MeetingID()
	isHost, err := h.store.IsMeetingHost(meetingID, session.UserID())
	if err != nil || !isHost {
		_ = session.Send(protocol.MeetMuteAllRsp, &protocol.MeetMuteAllRspBody{Success: false})
		return
	}

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetMuteAllReq, &protocol.MeetMuteAllReqBody{Mute: req.Mute}, session.UserID())
	_ = session.Send(protocol.MeetMuteAllRsp, &protocol.MeetMuteAllRspBody{Success: true})
}

func (h *MeetingHandler) LeaveByDisconnect(session *server.Session) {
	h.leave(session, false, "网络断开")
}

func (h *MeetingHandler) leave(session *server.Session, reply bool, reason string) {
	meetingID := session.MeetingID()
	if meetingID == "" {
		if reply {
			_ = session.Send(protocol.MeetLeaveRsp, &protocol.MeetLeaveRspBody{Success: true})
		}
		return
	}

	userID := session.UserID()
	hostChanged, newHost, remaining, err := h.store.LeaveMeeting(meetingID, userID)
	if err == nil {
		_ = h.roomState.RemoveMember(context.Background(), meetingID, userID)

		h.sessions.BroadcastToRoom(meetingID, protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: userID, Reason: reason}, userID)
		if hostChanged && newHost != nil {
			_ = h.roomState.TransferHost(context.Background(), meetingID, newHost.UserId)
			h.sessions.BroadcastToRoom(meetingID, protocol.MeetHostChanged, &protocol.MeetHostChangedNotifyBody{NewHostId: newHost.UserId, NewHostName: newHost.DisplayName}, "")
		}

		if remaining == 0 {
			_ = h.roomState.DeleteRoom(context.Background(), meetingID)
		}
	}

	session.SetMeetingID("")
	session.SetState(server.StateAuthenticated)

	if reply {
		_ = session.Send(protocol.MeetLeaveRsp, &protocol.MeetLeaveRspBody{Success: true})
	}
}

func (h *MeetingHandler) replyKickError(session *server.Session, code int32, message string) {
	_ = session.Send(protocol.MeetKickRsp, &protocol.MeetKickRspBody{
		Success: false,
		Error: &protocol.ErrorInfo{
			Code:    code,
			Message: message,
		},
	})
}

func cloneParticipants(src []*protocol.Participant) []*protocol.Participant {
	if len(src) == 0 {
		return nil
	}

	out := make([]*protocol.Participant, 0, len(src))
	for _, p := range src {
		out = append(out, cloneParticipant(p))
	}
	return out
}

func cloneParticipant(src *protocol.Participant) *protocol.Participant {
	if src == nil {
		return nil
	}

	cloned, ok := proto.Clone(src).(*protocol.Participant)
	if !ok {
		return &protocol.Participant{
			UserId:      src.UserId,
			DisplayName: src.DisplayName,
			AvatarUrl:   src.AvatarUrl,
			Role:        src.Role,
			IsAudioOn:   src.IsAudioOn,
			IsVideoOn:   src.IsVideoOn,
			IsSharing:   src.IsSharing,
		}
	}
	return cloned
}
