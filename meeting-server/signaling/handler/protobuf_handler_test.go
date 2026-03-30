package handler

import (
	"io"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"net"
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
	h := NewMeetingHandler(cfg, sessions, memStore, roomState)

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
	h := NewMeetingHandler(cfg, sessions, memStore, roomState)

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
	h := NewMeetingHandler(cfg, sessions, memStore, roomState)

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
