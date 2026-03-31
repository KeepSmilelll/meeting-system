package main

import (
	"io"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/handler"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"net"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"
)

func newPipeSession(t *testing.T, id uint64, cfg config.Config) (*server.Session, net.Conn, func()) {
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

func readSignalFrame(t *testing.T, conn net.Conn, maxPayload uint32) (protocol.SignalType, []byte) {
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

func assertNoSignalFrame(t *testing.T, conn net.Conn) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 1)
	_, err := conn.Read(buf)
	if err == nil {
		t.Fatalf("expected no frame, but read data")
	}
	if ne, ok := err.(net.Error); ok && ne.Timeout() {
		return
	}
	if err != io.EOF {
		t.Fatalf("expected timeout or eof, got %v", err)
	}
}

func TestMediaRouterForwardsOfferAnswerAndIceCandidate(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := handler.NewRouter(cfg, sessions, memStore, memStore, roomState, tokenManager, limiter)

	senderSess, senderConn, senderCleanup := newPipeSession(t, 401, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("meeting-media")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newPipeSession(t, 402, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("meeting-media")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u1002", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, payload := readSignalFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	var offer protocol.MediaOfferBody
	if err := proto.Unmarshal(payload, &offer); err != nil {
		t.Fatalf("unmarshal offer failed: %v", err)
	}
	if offer.TargetUserId != "u1002" || offer.Sdp != "offer-sdp" {
		t.Fatalf("unexpected offer payload: %+v", offer)
	}
	assertNoSignalFrame(t, senderConn)

	answerPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1001", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal answer failed: %v", err)
	}
	router.HandleMessage(targetSess, protocol.MediaAnswer, answerPayload)

	msgType, payload = readSignalFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	var answer protocol.MediaAnswerBody
	if err := proto.Unmarshal(payload, &answer); err != nil {
		t.Fatalf("unmarshal answer failed: %v", err)
	}
	if answer.TargetUserId != "u1001" || answer.Sdp != "answer-sdp" {
		t.Fatalf("unexpected answer payload: %+v", answer)
	}
	assertNoSignalFrame(t, targetConn)

	candidatePayload, err := proto.Marshal(&protocol.MediaIceCandidateBody{TargetUserId: "u1002", Candidate: "cand", SdpMid: "0", SdpMlineIndex: 1})
	if err != nil {
		t.Fatalf("marshal candidate failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaIceCandidate, candidatePayload)

	msgType, payload = readSignalFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaIceCandidate {
		t.Fatalf("unexpected candidate type: got %v want %v", msgType, protocol.MediaIceCandidate)
	}
	var candidate protocol.MediaIceCandidateBody
	if err := proto.Unmarshal(payload, &candidate); err != nil {
		t.Fatalf("unmarshal candidate failed: %v", err)
	}
	if candidate.TargetUserId != "u1002" || candidate.Candidate != "cand" || candidate.SdpMid != "0" || candidate.SdpMlineIndex != 1 {
		t.Fatalf("unexpected candidate payload: %+v", candidate)
	}
}

func TestMediaRouterRejectsInvalidTargets(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := handler.NewRouter(cfg, sessions, memStore, memStore, roomState, tokenManager, limiter)

	senderSess, senderConn, senderCleanup := newPipeSession(t, 403, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("meeting-a")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newPipeSession(t, 404, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("meeting-b")
	targetSess.SetState(server.StateInMeeting)

	emptyTargetPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal empty target failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, emptyTargetPayload)
	assertNoSignalFrame(t, targetConn)
	assertNoSignalFrame(t, senderConn)

	wrongRoomPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1002", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal wrong room failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaAnswer, wrongRoomPayload)
	assertNoSignalFrame(t, targetConn)
	assertNoSignalFrame(t, senderConn)
}
