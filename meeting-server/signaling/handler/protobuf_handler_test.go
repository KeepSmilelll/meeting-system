package handler

import (
	"context"
	"io"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"net"
	"sync"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
)

func newRunningSession(t *testing.T, id uint64, cfg config.Config) (*server.Session, net.Conn, func()) {
	t.Helper()

	srv, cli := net.Pipe()
	sess := server.NewSession(id, srv, cfg)
	sess.Start(func(*server.Session, protocol.SignalType, []byte) {}, func(*server.Session) {})

	cleanup := func() {
		sess.Close()
		_ = srv.Close()
		_ = cli.Close()
	}

	return sess, cli, cleanup
}

func readFrame(t *testing.T, conn net.Conn, maxPayload uint32) (protocol.SignalType, []byte) {
	t.Helper()

	header := make([]byte, protocol.HeaderSize)
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("read header failed: %v", err)
	}

	msgType, payloadLen, err := protocol.DecodeHeader(header, maxPayload)
	if err != nil {
		t.Fatalf("decode header failed: %v", err)
	}

	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(conn, payload); err != nil {
			t.Fatalf("read payload failed: %v", err)
		}
	}

	return msgType, payload
}

func TestAuthHandlerResumeTokenRestoreWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(sessions, memStore, tokenManager, limiter)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 101, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", ResumeToken: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected resume token success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId != "u1001" || rsp.Token == "" {
		t.Fatalf("expected restored session for u1001, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
}

func TestAuthHandlerResumeTokenUsernameMismatchFails(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(sessions, memStore, tokenManager, limiter)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 102, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "alice", ResumeToken: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if rsp.Success {
		t.Fatalf("expected resume token mismatch failure, got success: %+v", rsp)
	}
	if rsp.Error == nil || rsp.Error.Code != protocol.ErrAuthFailed {
		t.Fatalf("expected auth failed error, got %+v", rsp.Error)
	}
}

func TestAuthHandlerLegacyTokenRestoreWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(sessions, memStore, tokenManager, limiter)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 103, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", PasswordHash: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected legacy token restore success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId != "u1001" || rsp.Token == "" {
		t.Fatalf("expected restored session for u1001, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
}

func TestAuthHandlerLoginWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(sessions, memStore, tokenManager, limiter)

	sess, client, cleanup := newRunningSession(t, 1, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", PasswordHash: "demo"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected login success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId == "" || rsp.Token == "" {
		t.Fatalf("expected non-empty user/token, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
	if sess.State() != server.StateAuthenticated {
		t.Fatalf("expected authenticated state, got %v", sess.State())
	}
}

func TestMeetingHandlerCreateWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	sess, client, cleanup := newRunningSession(t, 2, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	req := &protocol.MeetCreateReqBody{Title: "proto-room", MaxParticipants: 3}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType1, rspPayload1 := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected first msg type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	var createRsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(rspPayload1, &createRsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if !createRsp.Success || createRsp.MeetingId == "" {
		t.Fatalf("expected create success with meeting id, got %+v", createRsp)
	}

	msgType2, rspPayload2 := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected second msg type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(rspPayload2, &joinRsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if !joinRsp.Success || len(joinRsp.Participants) != 1 {
		t.Fatalf("expected auto-join success with 1 participant, got %+v", joinRsp)
	}
	if joinRsp.Participants[0].UserId != "u1001" {
		t.Fatalf("unexpected host user id: %q", joinRsp.Participants[0].UserId)
	}
	if sess.State() != server.StateInMeeting {
		t.Fatalf("expected in-meeting state, got %v", sess.State())
	}
}

func TestMeetingHandlerJoinBroadcastParticipantNotify(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	meeting, _, err := memStore.CreateMeeting("join-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 4, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 5, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	req := &protocol.MeetJoinReqBody{MeetingId: meeting.ID}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}

	h.HandleJoin(peerSess, payload)

	joinType, joinPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected peer join rsp type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
		t.Fatalf("unmarshal join rsp failed: %v", err)
	}
	if !joinRsp.Success || len(joinRsp.Participants) != 2 {
		t.Fatalf("expected join success with 2 participants, got %+v", joinRsp)
	}

	notifyType, notifyPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if notifyType != protocol.MeetParticipantJoin {
		t.Fatalf("unexpected host notify type: got %v want %v", notifyType, protocol.MeetParticipantJoin)
	}
	var notify protocol.MeetParticipantJoinNotifyBody
	if err := proto.Unmarshal(notifyPayload, &notify); err != nil {
		t.Fatalf("unmarshal join notify failed: %v", err)
	}
	if notify.Participant == nil || notify.Participant.UserId != "u1002" {
		t.Fatalf("unexpected join notify payload: %+v", notify)
	}
}

type meetingMirrorCall struct {
	kind      string
	meetingID string
	userID    string
	reason    string
	title     string
}

type recordingMeetingMirror struct {
	mu    sync.Mutex
	calls []meetingMirrorCall
}

func (m *recordingMeetingMirror) append(call meetingMirrorCall) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.calls = append(m.calls, call)
}

func (m *recordingMeetingMirror) snapshot() []meetingMirrorCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]meetingMirrorCall, len(m.calls))
	copy(out, m.calls)
	return out
}

func (m *recordingMeetingMirror) MirrorCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error {
	_ = ctx
	call := meetingMirrorCall{kind: "create"}
	if meeting != nil {
		call.meetingID = meeting.ID
		call.title = meeting.Title
	}
	if host != nil {
		call.userID = host.UserId
	}
	m.append(call)
	return nil
}

func (m *recordingMeetingMirror) MirrorJoin(ctx context.Context, meeting *store.Meeting, participant *protocol.Participant) error {
	_ = ctx
	call := meetingMirrorCall{kind: "join"}
	if meeting != nil {
		call.meetingID = meeting.ID
		call.title = meeting.Title
	}
	if participant != nil {
		call.userID = participant.UserId
	}
	m.append(call)
	return nil
}

func (m *recordingMeetingMirror) MirrorLeave(ctx context.Context, meetingID, userID, reason string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "leave", meetingID: meetingID, userID: userID, reason: reason})
	return nil
}

func (m *recordingMeetingMirror) MirrorTransferHost(ctx context.Context, meetingID, newHostUserID string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "transfer", meetingID: meetingID, userID: newHostUserID})
	return nil
}

func (m *recordingMeetingMirror) MirrorDelete(ctx context.Context, meetingID string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "delete", meetingID: meetingID})
	return nil
}

func TestMeetingHandlerMirrorCallbacksOnLifecycle(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	recorder := &recordingMeetingMirror{}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, recorder)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 61, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "mirror-room", MaxParticipants: 4})
	if err != nil {
		t.Fatalf("marshal create req failed: %v", err)
	}
	h.HandleCreate(hostSess, createPayload)

	msgType1, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create rsp type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	msgType2, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join rsp type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}

	calls := recorder.snapshot()
	if len(calls) != 1 || calls[0].kind != "create" {
		t.Fatalf("expected create mirror call, got %+v", calls)
	}
	meetingID := calls[0].meetingID
	if meetingID == "" {
		t.Fatalf("expected create mirror call to include meeting id, got %+v", calls)
	}

	peerSess, peerConn, peerCleanup := newRunningSession(t, 62, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: meetingID})
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}
	h.HandleJoin(peerSess, joinPayload)

	joinType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join rsp type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	calls = recorder.snapshot()
	if len(calls) != 2 || calls[1].kind != "join" || calls[1].userID != "u1002" || calls[1].meetingID != meetingID {
		t.Fatalf("expected peer join mirror call, got %+v", calls)
	}

	leavePayload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}
	h.HandleLeave(peerSess, leavePayload)

	leaveType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if leaveType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected leave rsp type: got %v want %v", leaveType, protocol.MeetLeaveRsp)
	}
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	calls = recorder.snapshot()
	if len(calls) != 3 || calls[2].kind != "leave" || calls[2].reason != "主动离开" {
		t.Fatalf("expected leave mirror call, got %+v", calls)
	}
}

func TestMeetingHandlerMirrorCallbacksOnKick(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	recorder := &recordingMeetingMirror{}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, recorder)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 63, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "kick-mirror", MaxParticipants: 4})
	if err != nil {
		t.Fatalf("marshal create req failed: %v", err)
	}
	h.HandleCreate(hostSess, createPayload)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	calls := recorder.snapshot()
	meetingID := calls[0].meetingID

	peerSess, peerConn, peerCleanup := newRunningSession(t, 64, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: meetingID})
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}
	h.HandleJoin(peerSess, joinPayload)
	_, _ = readFrame(t, peerConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	kickPayload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal kick req failed: %v", err)
	}
	h.HandleKick(hostSess, kickPayload)

	kickType, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if kickType == protocol.MeetParticipantLeave {
		kickType, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	}
	if kickType != protocol.MeetKickRsp {
		t.Fatalf("unexpected kick rsp type: got %v want %v", kickType, protocol.MeetKickRsp)
	}
	leaveType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if leaveType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected kick leave notify type: got %v want %v", leaveType, protocol.MeetParticipantLeave)
	}

	calls = recorder.snapshot()
	if len(calls) != 3 || calls[2].kind != "leave" || calls[2].reason != "被主持人移出会议" {
		t.Fatalf("expected kick mirror leave call, got %+v", calls)
	}
}
func TestChatHandlerSendWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	h := NewChatHandler(cfg, sessions, memStore)

	sess, client, cleanup := newRunningSession(t, 3, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetMeetingID("m-test")
	sess.SetState(server.StateInMeeting)

	req := &protocol.ChatSendReqBody{Type: 1, Content: "hello protobuf"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleSend(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.ChatSendRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.ChatSendRsp)
	}

	var rsp protocol.ChatSendRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}
	if !rsp.Success || rsp.MessageId == "" {
		t.Fatalf("expected chat send success with message id, got %+v", rsp)
	}
}

func TestMeetingHandlerKickWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	meeting, _, err := memStore.CreateMeeting("kick-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 11, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 12, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	req := &protocol.MeetKickReqBody{TargetUserId: "u1002"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal kick req failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	msgType1, payload1 := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType1 == protocol.MeetParticipantLeave {
		msgType1, payload1 = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	}
	if msgType1 != protocol.MeetKickRsp {
		t.Fatalf("unexpected host msg type: got %v want %v", msgType1, protocol.MeetKickRsp)
	}
	var kickRsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(payload1, &kickRsp); err != nil {
		t.Fatalf("unmarshal kick rsp failed: %v", err)
	}
	if !kickRsp.Success {
		t.Fatalf("expected kick success, got %+v", kickRsp)
	}

	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected target notify type: got %v want %v", targetType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(targetPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal target leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" {
		t.Fatalf("unexpected leave user: %s", leaveNotify.UserId)
	}

	if targetSess.State() != server.StateAuthenticated || targetSess.MeetingID() != "" {
		t.Fatalf("expected kicked session reset, state=%v meeting=%q", targetSess.State(), targetSess.MeetingID())
	}
}

func TestMeetingHandlerMuteAllWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	meeting, _, err := memStore.CreateMeeting("mute-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 21, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 22, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	req := &protocol.MeetMuteAllReqBody{Mute: true}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal mute req failed: %v", err)
	}

	h.HandleMuteAll(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetMuteAllRsp {
		t.Fatalf("unexpected host mute rsp type: got %v want %v", hostType, protocol.MeetMuteAllRsp)
	}
	var muteRsp protocol.MeetMuteAllRspBody
	if err := proto.Unmarshal(hostPayload, &muteRsp); err != nil {
		t.Fatalf("unmarshal mute rsp failed: %v", err)
	}
	if !muteRsp.Success {
		t.Fatalf("expected mute all success")
	}

	peerType, peerPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType != protocol.MeetMuteAllReq {
		t.Fatalf("unexpected peer notify type: got %v want %v", peerType, protocol.MeetMuteAllReq)
	}
	var muteNotify protocol.MeetMuteAllReqBody
	if err := proto.Unmarshal(peerPayload, &muteNotify); err != nil {
		t.Fatalf("unmarshal mute notify failed: %v", err)
	}
	if !muteNotify.Mute {
		t.Fatalf("expected mute=true notify")
	}
}

func TestMeetingHandlerLeaveWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	meeting, _, err := memStore.CreateMeeting("leave-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 31, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 32, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(peerSess, payload)

	peerType, peerPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected peer leave rsp type: got %v want %v", peerType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(peerPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected leave success")
	}

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected host notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "主动离开" {
		t.Fatalf("unexpected leave notify: %+v", leaveNotify)
	}

	if peerSess.State() != server.StateAuthenticated || peerSess.MeetingID() != "" {
		t.Fatalf("expected leaving session reset, state=%v meeting=%q", peerSess.State(), peerSess.MeetingID())
	}
	if exists, err := memStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected participant removed, exists=%v err=%v", exists, err)
	}
}

func TestMeetingHandlerLeaveTransfersHostAndCleansEmptyRoom(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil)

	meeting, _, err := memStore.CreateMeeting("host-leave", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting u1002 failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1003"); err != nil {
		t.Fatalf("join meeting u1003 failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 41, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 42, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	thirdSess, thirdConn, thirdCleanup := newRunningSession(t, 43, cfg)
	defer thirdCleanup()
	sessions.Add(thirdSess)
	sessions.BindUser(thirdSess, "u1003")
	thirdSess.SetMeetingID(meeting.ID)
	thirdSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected host leave rsp type: got %v want %v", hostType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(hostPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal host leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected host leave success")
	}

	peerType1, peerPayload1 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected peer leave notify type: got %v want %v", peerType1, protocol.MeetParticipantLeave)
	}
	var peerLeave protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(peerPayload1, &peerLeave); err != nil {
		t.Fatalf("unmarshal peer leave notify failed: %v", err)
	}
	if peerLeave.UserId != "u1001" {
		t.Fatalf("unexpected leave user: %+v", peerLeave)
	}

	peerType2, peerPayload2 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected peer host-changed type: got %v want %v", peerType2, protocol.MeetHostChanged)
	}
	var hostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(peerPayload2, &hostChanged); err != nil {
		t.Fatalf("unmarshal host changed notify failed: %v", err)
	}
	if hostChanged.NewHostId != "u1002" {
		t.Fatalf("expected u1002 to become host, got %+v", hostChanged)
	}

	thirdType1, _ := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected third leave notify type: got %v want %v", thirdType1, protocol.MeetParticipantLeave)
	}
	thirdType2, thirdPayload2 := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected third host-changed type: got %v want %v", thirdType2, protocol.MeetHostChanged)
	}
	var thirdHostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(thirdPayload2, &thirdHostChanged); err != nil {
		t.Fatalf("unmarshal third host changed notify failed: %v", err)
	}
	if thirdHostChanged.NewHostId != "u1002" {
		t.Fatalf("expected u1002 host change for all peers, got %+v", thirdHostChanged)
	}

	if isHost, err := memStore.IsMeetingHost(meeting.ID, "u1002"); err != nil || !isHost {
		t.Fatalf("expected u1002 to be stored as host, isHost=%v err=%v", isHost, err)
	}

	cleanupMeeting, _, err := memStore.CreateMeeting("cleanup-room", "", "u1003", 1)
	if err != nil {
		t.Fatalf("create cleanup meeting failed: %v", err)
	}
	lastSess, lastConn, lastCleanup := newRunningSession(t, 44, cfg)
	defer lastCleanup()
	sessions.Add(lastSess)
	sessions.BindUser(lastSess, "u1003")
	lastSess.SetMeetingID(cleanupMeeting.ID)
	lastSess.SetState(server.StateInMeeting)

	h.HandleLeave(lastSess, payload)

	lastType, lastPayload := readFrame(t, lastConn, cfg.MaxPayloadBytes)
	if lastType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected cleanup leave rsp type: got %v want %v", lastType, protocol.MeetLeaveRsp)
	}
	if err := proto.Unmarshal(lastPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal cleanup leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected cleanup leave success")
	}
	if _, err := memStore.Participants(cleanupMeeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected empty room cleanup, got %v", err)
	}
}

func TestRouterOnSessionClosedTriggersDisconnectLeave(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, tokenManager, limiter)

	meeting, _, err := memStore.CreateMeeting("disconnect-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 51, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, _, peerCleanup := newRunningSession(t, 52, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateDisconnected)

	router.OnSessionClosed(peerSess)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected disconnect notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal disconnect leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "网络断开" {
		t.Fatalf("unexpected disconnect leave notify: %+v", leaveNotify)
	}

	if peerSess.State() != server.StateDisconnected {
		t.Fatalf("expected disconnected session state to be preserved, got %v", peerSess.State())
	}
	if peerSess.MeetingID() != "" {
		t.Fatalf("expected disconnected session meeting to be cleared, got %q", peerSess.MeetingID())
	}
	if exists, err := memStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected disconnected participant removed, exists=%v err=%v", exists, err)
	}
}

