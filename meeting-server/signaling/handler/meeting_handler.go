package handler

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	pb "meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

type MeetingHandler struct {
	cfg          config.Config
	sessions     *server.SessionManager
	store        store.MeetingLifecycleStore
	roomState    *store.RedisRoomStore
	sessionStore store.SessionStore
	nodeBus      store.UserEventPublisher
	sfuClient    signalingSfu.Client
	mirror       MeetingMirror
}

func NewMeetingHandler(cfg config.Config, sessions *server.SessionManager, meetingStore store.MeetingLifecycleStore, roomState *store.RedisRoomStore, sessionStore store.SessionStore, nodeBus store.UserEventPublisher, sfuClient signalingSfu.Client, mirrors ...MeetingMirror) *MeetingHandler {
	return &MeetingHandler{
		cfg:          cfg,
		sessions:     sessions,
		store:        meetingStore,
		roomState:    roomState,
		sessionStore: sessionStore,
		nodeBus:      nodeBus,
		sfuClient:    sfuClient,
		mirror:       ComposeMeetingMirrors(mirrors...),
	}
}

func (h *MeetingHandler) HandleCreate(session *server.Session, payload []byte) {
	var req protocol.MeetCreateReqBody
	if !decodeProto(payload, &req) || req.Title == "" {
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	meeting, hostParticipant, err := h.store.CreateMeeting(req.Title, req.Password, session.UserID(), int(req.MaxParticipants))
	if err != nil {
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: mapCreateErrorCode(err), Message: err.Error()},
		})
		return
	}

	ctx := context.Background()
	sfuNodeID, sfuAddress, err := h.allocateRoomRoute(ctx, meeting)
	if err != nil {
		h.rollbackCreate(ctx, meeting.ID, session.UserID())
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: protocol.ErrInternal, Message: err.Error()},
		})
		return
	}
	if err := h.roomState.UpsertRoom(ctx, meeting.ID, hostParticipant.UserId, sfuNodeID, sfuAddress); err != nil {
		h.rollbackCreate(ctx, meeting.ID, session.UserID())
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: protocol.ErrInternal, Message: err.Error()},
		})
		return
	}
	if err := h.roomState.AddMember(ctx, meeting.ID, hostParticipant); err != nil {
		_ = h.roomState.DeleteRoom(ctx, meeting.ID)
		h.rollbackCreate(ctx, meeting.ID, session.UserID())
		_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: protocol.ErrInternal, Message: err.Error()},
		})
		return
	}

	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)
	syncSessionPresence(ctx, h.cfg, h.sessionStore, session)

	_ = h.mirrorMeetingCreate(ctx, meeting, hostParticipant)

	_ = session.Send(protocol.MeetCreateRsp, &protocol.MeetCreateRspBody{Success: true, MeetingId: meeting.ID})
	_ = session.Send(protocol.MeetJoinRsp, h.buildJoinResponse(meeting, []*protocol.Participant{hostParticipant}, session.UserID()))
}

func (h *MeetingHandler) HandleJoin(session *server.Session, payload []byte) {
	var req protocol.MeetJoinReqBody
	if !decodeProto(payload, &req) || req.MeetingId == "" {
		_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{Success: false, Error: &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad request"}})
		return
	}

	meeting, participants, joined, err := h.store.JoinMeeting(req.MeetingId, req.Password, session.UserID())
	if err != nil {
		_ = session.Send(protocol.MeetJoinRsp, &protocol.MeetJoinRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: mapJoinErrorCode(err), Message: err.Error()},
		})
		return
	}

	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)

	ctx := context.Background()
	syncSessionPresence(ctx, h.cfg, h.sessionStore, session)
	_ = h.roomState.AddMember(ctx, meeting.ID, joined)
	_ = h.mirrorMeetingJoin(ctx, meeting, joined)

	_ = session.Send(protocol.MeetJoinRsp, h.buildJoinResponse(meeting, participants, session.UserID()))

	h.sessions.BroadcastToRoom(
		meeting.ID,
		protocol.MeetParticipantJoin,
		&protocol.MeetParticipantJoinNotifyBody{Participant: cloneParticipant(joined)},
		session.UserID(),
	)
	h.broadcastMeetingStateSync(meeting.ID, "")
	h.replayMuteAllState(session, meeting.ID)
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
		h.replyKickError(session, mapKickStoreErrorCode(err), err.Error())
		return
	}
	if !isHost {
		h.replyKickError(session, protocol.ErrNotHost, "only host can kick")
		return
	}

	exists, err := h.store.HasParticipant(meetingID, req.TargetUserId)
	if err != nil {
		h.replyKickError(session, mapKickStoreErrorCode(err), err.Error())
		return
	}
	if !exists {
		h.replyKickError(session, protocol.ErrMeetingNotFound, "target not in meeting")
		return
	}

	hostChanged, newHost, remaining, err := h.store.LeaveMeeting(meetingID, req.TargetUserId)
	if err != nil {
		h.replyKickError(session, mapKickStoreErrorCode(err), err.Error())
		return
	}

	ctx := context.Background()
	_ = h.roomState.RemoveMember(ctx, meetingID, req.TargetUserId)
	_ = h.removeSFUPublisher(ctx, meetingID, req.TargetUserId)
	_ = h.mirrorMeetingLeave(ctx, meetingID, req.TargetUserId, "被主持人移出会议")

	reason := "被主持人移出会议"
	if kickedSession, ok := h.sessions.GetByUser(req.TargetUserId); ok && kickedSession.MeetingID() == meetingID {
		kickedSession.SetMeetingID("")
		kickedSession.SetState(server.StateAuthenticated)
		syncSessionPresence(ctx, h.cfg, h.sessionStore, kickedSession)
		_ = kickedSession.Send(protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: req.TargetUserId, Reason: reason})
	} else {
		h.kickRemoteParticipant(ctx, meetingID, req.TargetUserId, reason)
	}

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: req.TargetUserId, Reason: reason}, req.TargetUserId)
	if hostChanged && newHost != nil {
		_ = h.roomState.TransferHost(ctx, meetingID, newHost.UserId)
		_ = h.mirrorMeetingTransferHost(ctx, meetingID, newHost.UserId)
		h.sessions.BroadcastToRoom(meetingID, protocol.MeetHostChanged, &protocol.MeetHostChangedNotifyBody{NewHostId: newHost.UserId, NewHostName: newHost.DisplayName}, "")
	}
	if remaining > 0 {
		h.broadcastMeetingStateSync(meetingID, req.TargetUserId)
	}
	if remaining == 0 {
		_ = h.destroySFURoom(ctx, meetingID)
		_ = h.roomState.DeleteRoom(ctx, meetingID)
		_ = h.mirrorMeetingDelete(ctx, meetingID)
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

	if err := h.roomState.SetMuteAll(context.Background(), meetingID, req.Mute); err != nil {
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
	ctx := context.Background()
	hostChanged, newHost, remaining, err := h.store.LeaveMeeting(meetingID, userID)
	if err != nil {
		if reply {
			_ = session.Send(protocol.MeetLeaveRsp, &protocol.MeetLeaveRspBody{Success: false})
		}
		return
	}

	_ = h.roomState.RemoveMember(ctx, meetingID, userID)
	_ = h.removeSFUPublisher(ctx, meetingID, userID)
	_ = h.mirrorMeetingLeave(ctx, meetingID, userID, reason)

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetParticipantLeave, &protocol.MeetParticipantLeaveNotifyBody{UserId: userID, Reason: reason}, userID)
	if hostChanged && newHost != nil {
		_ = h.roomState.TransferHost(ctx, meetingID, newHost.UserId)
		_ = h.mirrorMeetingTransferHost(ctx, meetingID, newHost.UserId)
		h.sessions.BroadcastToRoom(meetingID, protocol.MeetHostChanged, &protocol.MeetHostChangedNotifyBody{NewHostId: newHost.UserId, NewHostName: newHost.DisplayName}, userID)
	}
	if remaining > 0 {
		h.broadcastMeetingStateSync(meetingID, userID)
	}

	if remaining == 0 {
		_ = h.destroySFURoom(ctx, meetingID)
		_ = h.roomState.DeleteRoom(ctx, meetingID)
		_ = h.mirrorMeetingDelete(ctx, meetingID)
	}

	session.SetMeetingID("")
	if session.State() != server.StateDisconnected {
		if reply {
			session.SetState(server.StateAuthenticated)
		}
		syncSessionPresence(ctx, h.cfg, h.sessionStore, session)
	}
	if reply {
		_ = session.Send(protocol.MeetLeaveRsp, &protocol.MeetLeaveRspBody{Success: true})
	}
}

func (h *MeetingHandler) mirrorMeetingCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error {
	if h == nil || h.mirror == nil || meeting == nil {
		return nil
	}

	meetingCopy := *meeting
	return h.mirror.MirrorCreate(ctx, &meetingCopy, cloneParticipant(host))
}

func (h *MeetingHandler) mirrorMeetingJoin(ctx context.Context, meeting *store.Meeting, participant *protocol.Participant) error {
	if h == nil || h.mirror == nil || meeting == nil {
		return nil
	}

	meetingCopy := *meeting
	return h.mirror.MirrorJoin(ctx, &meetingCopy, cloneParticipant(participant))
}

func (h *MeetingHandler) mirrorMeetingLeave(ctx context.Context, meetingID, userID, reason string) error {
	if h == nil || h.mirror == nil || meetingID == "" || userID == "" {
		return nil
	}

	return h.mirror.MirrorLeave(ctx, meetingID, userID, reason)
}

func (h *MeetingHandler) mirrorMeetingTransferHost(ctx context.Context, meetingID, newHostUserID string) error {
	if h == nil || h.mirror == nil || meetingID == "" || newHostUserID == "" {
		return nil
	}

	return h.mirror.MirrorTransferHost(ctx, meetingID, newHostUserID)
}

func (h *MeetingHandler) mirrorMeetingDelete(ctx context.Context, meetingID string) error {
	if h == nil || h.mirror == nil || meetingID == "" {
		return nil
	}

	return h.mirror.MirrorDelete(ctx, meetingID)
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

func (h *MeetingHandler) kickRemoteParticipant(ctx context.Context, meetingID, targetUserID, reason string) {
	if h == nil || h.sessionStore == nil || h.nodeBus == nil {
		return
	}

	presence, err := h.sessionStore.Get(ctx, targetUserID)
	if err != nil || presence == nil {
		return
	}
	if presence.NodeID == "" || presence.NodeID == h.cfg.NodeID || presence.MeetingID != meetingID || presence.Status != int32(server.StateInMeeting) {
		return
	}

	nextState := int32(server.StateAuthenticated)
	_ = h.nodeBus.PublishUserControl(ctx, store.UserNodeEvent{
		TargetNodeID:    presence.NodeID,
		TargetUserID:    targetUserID,
		TargetSessionID: presence.SessionID,
		Frame:           protocol.EncodeFrame(protocol.MeetParticipantLeave, mustMarshalProto(&protocol.MeetParticipantLeaveNotifyBody{UserId: targetUserID, Reason: reason})),
		ResetMeeting:    true,
		State:           &nextState,
	})
}

func (h *MeetingHandler) broadcastMeetingStateSync(meetingID, excludeUserID string) {
	if h == nil || h.store == nil || h.sessions == nil || meetingID == "" {
		return
	}

	meeting, participants, err := h.store.SnapshotMeeting(meetingID)
	if err != nil || meeting == nil {
		return
	}
	participants = h.roomState.HydrateParticipants(context.Background(), meetingID, participants)

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetStateSync, &protocol.MeetStateSyncNotifyBody{
		MeetingId:    meeting.ID,
		Title:        meeting.Title,
		HostId:       meeting.HostUserID,
		Participants: cloneParticipants(participants),
	}, excludeUserID)
}

func (h *MeetingHandler) replayMuteAllState(session *server.Session, meetingID string) {
	if h == nil || session == nil || meetingID == "" {
		return
	}

	muted, err := h.roomState.MuteAll(context.Background(), meetingID)
	if err != nil || !muted {
		return
	}

	_ = session.Send(protocol.MeetMuteAllReq, &protocol.MeetMuteAllReqBody{Mute: true})
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

func (h *MeetingHandler) buildJoinResponse(meeting *store.Meeting, participants []*protocol.Participant, userID string) *protocol.MeetJoinRspBody {
	if meeting == nil {
		return &protocol.MeetJoinRspBody{Success: false}
	}
	ctx := context.Background()
	participants = h.roomState.HydrateParticipants(ctx, meeting.ID, participants)

	sfuAddress := h.cfg.DefaultSFUAddress
	if meta, err := h.roomState.RoomMetadata(ctx, meeting.ID); err == nil && meta != nil && meta.SFUAddress != "" {
		sfuAddress = meta.SFUAddress
	}

	return &protocol.MeetJoinRspBody{
		Success:      true,
		MeetingId:    meeting.ID,
		Title:        meeting.Title,
		SfuAddress:   sfuAddress,
		IceServers:   h.buildIceServers(userID),
		Participants: cloneParticipants(participants),
	}
}

func (h *MeetingHandler) buildIceServers(userID string) []*protocol.IceServer {
	if h == nil || len(h.cfg.TURNServers) == 0 {
		return nil
	}

	needsTURNCreds := false
	for _, url := range h.cfg.TURNServers {
		if isTURNURL(url) {
			needsTURNCreds = true
			break
		}
	}

	var username string
	var credential string
	if needsTURNCreds {
		var err error
		username, credential, err = auth.GenerateTURNCredentials(h.cfg.TURNSecret, userID, h.cfg.TURNCredTTL)
		if err != nil {
			return nil
		}
	}

	out := make([]*protocol.IceServer, 0, len(h.cfg.TURNServers))
	for _, url := range h.cfg.TURNServers {
		server := &protocol.IceServer{Urls: url}
		if isTURNURL(url) {
			server.Username = username
			server.Credential = credential
		}
		out = append(out, server)
	}
	return out
}

func isTURNURL(url string) bool {
	normalized := strings.ToLower(strings.TrimSpace(url))
	return strings.HasPrefix(normalized, "turn:") || strings.HasPrefix(normalized, "turns:")
}

func mapCreateErrorCode(err error) int32 {
	if isMeetingStoreNotFound(err) {
		return protocol.ErrMeetingNotFound
	}
	return protocol.ErrInternal
}

func mapJoinErrorCode(err error) int32 {
	switch {
	case errors.Is(err, store.ErrMeetingNotFound):
		return protocol.ErrMeetingNotFound
	case errors.Is(err, store.ErrMeetingFull):
		return protocol.ErrMeetingFull
	case errors.Is(err, store.ErrMeetingPassFailed):
		return protocol.ErrMeetingPassword
	default:
		return protocol.ErrInternal
	}
}

func mapKickStoreErrorCode(err error) int32 {
	if isMeetingStoreNotFound(err) {
		return protocol.ErrMeetingNotFound
	}
	return protocol.ErrInternal
}

func isMeetingStoreNotFound(err error) bool {
	return errors.Is(err, store.ErrMeetingNotFound)
}

func (h *MeetingHandler) allocateRoomRoute(ctx context.Context, meeting *store.Meeting) (string, string, error) {
	nodes := h.cfg.EffectiveSFUNodes()
	if h != nil && h.roomState != nil {
		configured := nodes
		if len(h.cfg.SFUNodes) == 0 {
			configured = nil
		}
		resolved, err := h.roomState.EffectiveSFUNodes(ctx, configured)
		if err != nil {
			return "", "", fmt.Errorf("resolve registered sfu nodes for meeting %s: %w", meeting.ID, err)
		}
		if len(resolved) > 0 {
			nodes = resolved
		}
	}
	if h == nil || h.sfuClient == nil || meeting == nil {
		if len(nodes) == 0 {
			return h.cfg.DefaultSFUNodeID, h.cfg.DefaultSFUAddress, nil
		}
		selected, err := h.roomState.NextSFUNode(ctx, nodes)
		if err != nil {
			return "", "", fmt.Errorf("select sfu node for meeting %s: %w", meeting.ID, err)
		}
		if selected == nil {
			return h.cfg.DefaultSFUNodeID, h.cfg.DefaultSFUAddress, nil
		}
		return selected.NodeID, selected.MediaAddress, nil
	}

	ranked, err := h.roomState.RankedSFUNodes(ctx, nodes)
	if err != nil {
		return "", "", fmt.Errorf("rank sfu nodes for meeting %s: %w", meeting.ID, err)
	}
	if len(ranked) == 0 {
		ranked = []config.SFUNode{{
			NodeID:       h.cfg.DefaultSFUNodeID,
			MediaAddress: h.cfg.DefaultSFUAddress,
			RPCAddress:   h.cfg.SFURPCAddr,
		}}
	}

	var lastErr error
	for _, candidate := range ranked {
		nodeID := candidate.NodeID
		sfuAddress := candidate.MediaAddress

		rsp, err := signalingSfu.ClientForRoute(h.sfuClient, sfuAddress).CreateRoom(ctx, &pb.CreateRoomReq{
			MeetingId:     meeting.ID,
			MaxPublishers: int32(meeting.MaxParticipants),
		})
		if errors.Is(err, signalingSfu.ErrDisabled) {
			return nodeID, sfuAddress, nil
		}
		if err != nil {
			lastErr = fmt.Errorf("create sfu room %s on node %s: %w", meeting.ID, nodeID, err)
			_ = h.roomState.MarkSFUNodeFailure(ctx, nodeID, h.cfg.SFUNodeQuarantine)
			continue
		}
		if rsp == nil || !rsp.GetSuccess() {
			lastErr = fmt.Errorf("create sfu room %s on node %s: unsuccessful response", meeting.ID, nodeID)
			_ = h.roomState.MarkSFUNodeFailure(ctx, nodeID, h.cfg.SFUNodeQuarantine)
			continue
		}
		_ = h.roomState.ClearSFUNodeFailure(ctx, nodeID)
		if rsp.GetSfuAddress() != "" {
			sfuAddress = rsp.GetSfuAddress()
		}
		return nodeID, sfuAddress, nil
	}
	if lastErr == nil {
		lastErr = fmt.Errorf("create sfu room %s: no available sfu nodes", meeting.ID)
	}
	return "", "", lastErr
}

func (h *MeetingHandler) rollbackCreate(ctx context.Context, meetingID, userID string) {
	if h == nil || h.store == nil || meetingID == "" || userID == "" {
		return
	}
	_, _, _, _ = h.store.LeaveMeeting(meetingID, userID)
	if h.roomState != nil {
		_ = h.roomState.DeleteRoom(ctx, meetingID)
	}
}

func (h *MeetingHandler) removeSFUPublisher(ctx context.Context, meetingID, userID string) error {
	if h == nil || h.sfuClient == nil || meetingID == "" || userID == "" {
		return nil
	}

	rsp, err := h.sfuClient.RemovePublisher(ctx, &pb.RemovePublisherReq{
		MeetingId: meetingID,
		UserId:    userID,
	})
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("remove sfu publisher meeting=%s user=%s: %w", meetingID, userID, err)
	}
	if rsp == nil || !rsp.GetSuccess() {
		return nil
	}
	return nil
}

func (h *MeetingHandler) destroySFURoom(ctx context.Context, meetingID string) error {
	if h == nil || h.sfuClient == nil || meetingID == "" {
		return nil
	}

	rsp, err := h.sfuClient.DestroyRoom(ctx, &pb.DestroyRoomReq{MeetingId: meetingID})
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("destroy sfu room %s: %w", meetingID, err)
	}
	if rsp == nil || !rsp.GetSuccess() {
		return fmt.Errorf("destroy sfu room %s: unsuccessful response", meetingID)
	}
	return nil
}
