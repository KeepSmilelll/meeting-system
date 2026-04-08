package store

import (
	"context"
	"testing"
	"time"

	"meeting-server/signaling/config"

	"github.com/alicebob/miniredis/v2"
)

type receivedUserFrame struct {
	userID          string
	targetSessionID uint64
	frame           []byte
	resetMeeting    bool
	close           bool
}

func TestRedisNodeEventBusPublishesAcrossNodes(t *testing.T) {
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

	busA := NewRedisNodeEventBusWithClient(cfgA, clientA)
	busB := NewRedisNodeEventBusWithClient(cfgB, clientB)
	defer busA.Close()
	defer busB.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	received := make(chan receivedUserFrame, 1)
	if err := busB.Start(ctx, func(event UserNodeEvent) {
		received <- receivedUserFrame{
			userID:          event.TargetUserID,
			targetSessionID: event.TargetSessionID,
			frame:           append([]byte(nil), event.Frame...),
			resetMeeting:    event.ResetMeeting,
			close:           event.Close,
		}
	}); err != nil {
		t.Fatalf("start node bus failed: %v", err)
	}

	frame := []byte{0xAB, 0xCD, 0x01}
	if err := busA.PublishUserFrame(context.Background(), "sig-node-b", "u1002", 202, frame); err != nil {
		t.Fatalf("publish user frame failed: %v", err)
	}

	select {
	case got := <-received:
		if got.userID != "u1002" || got.targetSessionID != 202 {
			t.Fatalf("unexpected target metadata: %+v", got)
		}
		if string(got.frame) != string(frame) {
			t.Fatalf("unexpected frame: %v", got.frame)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for node frame")
	}
}

func TestParseNodeFrameEnvelopeValidation(t *testing.T) {
	payload, err := marshalNodeFrameEnvelope(nodeFrameEnvelope{
		TargetUserID:    "u1002",
		TargetSessionID: "202",
		Frame:           []byte{1, 2, 3},
	})
	if err != nil {
		t.Fatalf("marshal node envelope failed: %v", err)
	}

	envelope, err := parseNodeFrameEnvelope(payload)
	if err != nil {
		t.Fatalf("parse node envelope failed: %v", err)
	}
	if envelope.TargetUserID != "u1002" || envelope.TargetSessionID != 202 {
		t.Fatalf("unexpected parsed envelope: %+v", envelope)
	}

	if _, err := parseNodeFrameEnvelope(`{"target_user_id":"u1002","frame":"AQID"}`); err == nil {
		t.Fatal("expected invalid envelope to fail")
	}

	state := int32(1)
	payload, err = marshalNodeFrameEnvelope(nodeFrameEnvelope{
		TargetUserID:    "u1002",
		TargetSessionID: "202",
		ResetMeeting:    true,
		State:           &state,
		Close:           true,
	})
	if err != nil {
		t.Fatalf("marshal node control envelope failed: %v", err)
	}
	envelope, err = parseNodeFrameEnvelope(payload)
	if err != nil {
		t.Fatalf("parse node control envelope failed: %v", err)
	}
	if !envelope.ResetMeeting || envelope.State == nil || *envelope.State != 1 || !envelope.Close {
		t.Fatalf("unexpected parsed control envelope: %+v", envelope)
	}
}
