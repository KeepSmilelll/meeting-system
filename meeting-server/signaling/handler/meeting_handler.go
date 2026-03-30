package handler

import (
    "meeting-server/signaling/config"
    "meeting-server/signaling/protocol"
    "meeting-server/signaling/server"
    "meeting-server/signaling/store"
)

type MeetingHandler struct {
    cfg      config.Config
    sessions *server.SessionManager
    store    *store.MemoryStore
}

func NewMeetingHandler(cfg config.Config, sessions *server.SessionManager, memStore *store.MemoryStore) *MeetingHandler {
    return &MeetingHandler{cfg: cfg, sessions: sessions, store: memStore}
}

func (h *MeetingHandler) HandleCreate(session *server.Session, payload []byte) {
    var req protocol.MeetCreateReqBody
    if !decodeJSON(payload, &req) || req.Title == "" {
        _ = session.Send(protocol.MeetCreateRsp, protocol.MeetCreateRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
        return
    }

    meeting, hostParticipant, err := h.store.CreateMeeting(req.Title, req.Password, session.UserID(), int(req.MaxParticipants))
    if err != nil {
        _ = session.Send(protocol.MeetCreateRsp, protocol.MeetCreateRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: err.Error()}})
        return
    }

    session.SetMeetingID(meeting.ID)
    session.SetState(server.StateInMeeting)

    _ = session.Send(protocol.MeetCreateRsp, protocol.MeetCreateRspBody{Success: true, MeetingID: meeting.ID})

    _ = session.Send(protocol.MeetJoinRsp, protocol.MeetJoinRspBody{
        Success:      true,
        MeetingID:    meeting.ID,
        Title:        meeting.Title,
        SFUAddress:   h.cfg.DefaultSFUAddress,
        Participants: []protocol.Participant{hostParticipant},
    })
}

func (h *MeetingHandler) HandleJoin(session *server.Session, payload []byte) {
    var req protocol.MeetJoinReqBody
    if !decodeJSON(payload, &req) || req.MeetingID == "" {
        _ = session.Send(protocol.MeetJoinRsp, protocol.MeetJoinRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
        return
    }

    meeting, participants, joined, err := h.store.JoinMeeting(req.MeetingID, req.Password, session.UserID())
    if err != nil {
        code := protocol.ErrMeetingNotFound
        switch err {
        case store.ErrMeetingFull:
            code = protocol.ErrMeetingFull
        case store.ErrMeetingPassFailed:
            code = protocol.ErrMeetingPassword
        }

        _ = session.Send(protocol.MeetJoinRsp, protocol.MeetJoinRspBody{Success: false, Error: &protocol.ErrorInfo{Code: code, Message: err.Error()}})
        return
    }

    session.SetMeetingID(meeting.ID)
    session.SetState(server.StateInMeeting)

    _ = session.Send(protocol.MeetJoinRsp, protocol.MeetJoinRspBody{
        Success:      true,
        MeetingID:    meeting.ID,
        Title:        meeting.Title,
        SFUAddress:   h.cfg.DefaultSFUAddress,
        Participants: participants,
    })

    h.sessions.BroadcastToMeeting(meeting.ID, protocol.MeetParticipantJoin, protocol.MeetParticipantJoinNotifyBody{Participant: joined}, session.UserID())
}

func (h *MeetingHandler) HandleLeave(session *server.Session, payload []byte) {
    _ = payload
    h.leave(session, true, "主动离开")
}

func (h *MeetingHandler) LeaveByDisconnect(session *server.Session) {
    h.leave(session, false, "网络断开")
}

func (h *MeetingHandler) leave(session *server.Session, reply bool, reason string) {
    meetingID := session.MeetingID()
    if meetingID == "" {
        if reply {
            _ = session.Send(protocol.MeetLeaveRsp, protocol.MeetLeaveRspBody{Success: true})
        }
        return
    }

    userID := session.UserID()
    hostChanged, newHost, _, err := h.store.LeaveMeeting(meetingID, userID)
    if err == nil {
        h.sessions.BroadcastToMeeting(meetingID, protocol.MeetParticipantLeave, protocol.MeetParticipantLeaveNotifyBody{UserID: userID, Reason: reason}, userID)
        if hostChanged {
            h.sessions.BroadcastToMeeting(meetingID, protocol.MeetHostChanged, protocol.MeetHostChangedNotifyBody{NewHostID: newHost.UserID, NewHostName: newHost.DisplayName}, "")
        }
    }

    session.SetMeetingID("")
    session.SetState(server.StateAuthenticated)

    if reply {
        _ = session.Send(protocol.MeetLeaveRsp, protocol.MeetLeaveRspBody{Success: true})
    }
}

