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

func assertNoFrame(t *testing.T, conn net.Conn, label string) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 1)
	_, err := conn.Read(buf)
	if err == nil {
		t.Fatalf("expected no frame on %s, but received data", label)
	}
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		// net.Pipe returns io.EOF after close; only a timeout means no frame arrived in time.
		if err != io.EOF {
			t.Fatalf("expected timeout on %s, got %v", label, err)
		}
	}
}

func TestRouterRoutesMediaOfferAnswerAndIceCandidate(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, tokenManager, limiter)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 201, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("m-media")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 202, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-media")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u1002", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, payload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	var offer protocol.MediaOfferBody
	if err := proto.Unmarshal(payload, &offer); err != nil {
		t.Fatalf("unmarshal offer failed: %v", err)
	}
	if offer.TargetUserId != "u1002" || offer.Sdp != "offer-sdp" {
		t.Fatalf("unexpected offer payload: %+v", offer)
	}
	assertNoFrame(t, senderConn, "sender on offer")

	answerPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1001", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal answer failed: %v", err)
	}
	router.HandleMessage(targetSess, protocol.MediaAnswer, answerPayload)

	msgType, payload = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected media answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	var answer protocol.MediaAnswerBody
	if err := proto.Unmarshal(payload, &answer); err != nil {
		t.Fatalf("unmarshal answer failed: %v", err)
	}
	if answer.TargetUserId != "u1001" || answer.Sdp != "answer-sdp" {
		t.Fatalf("unexpected answer payload: %+v", answer)
	}
	assertNoFrame(t, targetConn, "target on answer")

	candidatePayload, err := proto.Marshal(&protocol.MediaIceCandidateBody{TargetUserId: "u1002", Candidate: "cand", SdpMid: "0", SdpMlineIndex: 1})
	if err != nil {
		t.Fatalf("marshal candidate failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaIceCandidate, candidatePayload)

	msgType, payload = readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaIceCandidate {
		t.Fatalf("unexpected media candidate type: got %v want %v", msgType, protocol.MediaIceCandidate)
	}
	var candidate protocol.MediaIceCandidateBody
	if err := proto.Unmarshal(payload, &candidate); err != nil {
		t.Fatalf("unmarshal candidate failed: %v", err)
	}
	if candidate.TargetUserId != "u1002" || candidate.Candidate != "cand" || candidate.SdpMid != "0" || candidate.SdpMlineIndex != 1 {
		t.Fatalf("unexpected candidate payload: %+v", candidate)
	}
}

func TestRouterRejectsInvalidMediaTargets(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, tokenManager, limiter)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 301, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("m-a")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 302, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-b")
	targetSess.SetState(server.StateInMeeting)

	missingTargetPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal missing target payload failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, missingTargetPayload)
	assertNoFrame(t, targetConn, "target on missing target id")
	assertNoFrame(t, senderConn, "sender on missing target id")

	wrongMeetingPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1002", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal wrong meeting payload failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaAnswer, wrongMeetingPayload)
	assertNoFrame(t, targetConn, "target on wrong meeting")
	assertNoFrame(t, senderConn, "sender on wrong meeting")
}
