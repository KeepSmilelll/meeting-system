package store

import (
	"context"
	"testing"
	"time"

	"meeting-server/signaling/config"

	"github.com/alicebob/miniredis/v2"
)

type receivedMeetingFrame struct {
	meetingID     string
	frame         []byte
	excludeUserID string
}

func TestRedisRoomEventBusPublishesAcrossNodes(t *testing.T) {
	mini := miniredis.RunT(t)

	cfgA := config.Load()
	cfgA.EnableRedis = true
	cfgA.RedisAddr = mini.Addr()
	cfgA.NodeID = "sig-node-a"

	cfgB := cfgA
	cfgB.NodeID = "sig-node-b"

	clientA := NewRedisClient(cfgA)
	if clientA == nil {
		t.Fatal("expected redis client A")
	}
	defer clientA.Close()

	clientB := NewRedisClient(cfgB)
	if clientB == nil {
		t.Fatal("expected redis client B")
	}
	defer clientB.Close()

	busA := NewRedisRoomEventBusWithClient(cfgA, clientA)
	busB := NewRedisRoomEventBusWithClient(cfgB, clientB)
	defer busA.Close()
	defer busB.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	localA := make(chan receivedMeetingFrame, 1)
	remoteB := make(chan receivedMeetingFrame, 1)
	if err := busA.Start(ctx, func(meetingID string, frame []byte, excludeUserID string) {
		localA <- receivedMeetingFrame{meetingID: meetingID, frame: append([]byte(nil), frame...), excludeUserID: excludeUserID}
	}); err != nil {
		t.Fatalf("start busA failed: %v", err)
	}
	if err := busB.Start(ctx, func(meetingID string, frame []byte, excludeUserID string) {
		remoteB <- receivedMeetingFrame{meetingID: meetingID, frame: append([]byte(nil), frame...), excludeUserID: excludeUserID}
	}); err != nil {
		t.Fatalf("start busB failed: %v", err)
	}

	frame := []byte{0xAB, 0xCD, 0x01}
	if err := busA.PublishMeetingFrame(context.Background(), "m1001", frame, "u1002"); err != nil {
		t.Fatalf("publish meeting frame failed: %v", err)
	}

	select {
	case got := <-remoteB:
		if got.meetingID != "m1001" {
			t.Fatalf("unexpected meeting id: %q", got.meetingID)
		}
		if string(got.frame) != string(frame) {
			t.Fatalf("unexpected frame: %v", got.frame)
		}
		if got.excludeUserID != "u1002" {
			t.Fatalf("unexpected exclude user id: %q", got.excludeUserID)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for remote frame")
	}

	select {
	case got := <-localA:
		t.Fatalf("expected same-node publish to be filtered, got %+v", got)
	case <-time.After(200 * time.Millisecond):
	}
}

func TestParseRoomBroadcastMessageValidation(t *testing.T) {
	payload, err := marshalRoomBroadcastEnvelope(roomBroadcastEnvelope{
		NodeID:        "sig-node-a",
		ExcludeUserID: "u1002",
		Frame:         []byte{1, 2, 3},
	})
	if err != nil {
		t.Fatalf("marshal envelope failed: %v", err)
	}

	meetingID, envelope, err := parseRoomBroadcastMessage("sig:room:m1001", payload)
	if err != nil {
		t.Fatalf("parse room broadcast message failed: %v", err)
	}
	if meetingID != "m1001" {
		t.Fatalf("unexpected meeting id: %q", meetingID)
	}
	if envelope.NodeID != "sig-node-a" || envelope.ExcludeUserID != "u1002" {
		t.Fatalf("unexpected envelope metadata: %+v", envelope)
	}
	if string(envelope.Frame) != string([]byte{1, 2, 3}) {
		t.Fatalf("unexpected frame: %v", envelope.Frame)
	}

	if _, _, err := parseRoomBroadcastMessage("sig:node:sig-node-a", payload); err == nil {
		t.Fatal("expected invalid channel to fail")
	}
	if _, _, err := parseRoomBroadcastMessage("sig:room:m1001", `{"node_id":"sig-node-a"}`); err == nil {
		t.Fatal("expected empty frame to fail")
	}
}
