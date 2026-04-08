package store

import (
	"context"
	"testing"
	"time"

	"meeting-server/signaling/config"

	"github.com/alicebob/miniredis/v2"
)

func TestRedisSessionStoreUpsertGetAndDelete(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	store := NewRedisSessionStoreWithClient(cfg, client)
	ctx := context.Background()

	err := store.Upsert(ctx, SessionPresence{
		UserID:    "u1001",
		NodeID:    "sig-node-a",
		SessionID: 101,
		MeetingID: "m1001",
		Status:    2,
	}, 10*time.Minute)
	if err != nil {
		t.Fatalf("upsert session presence failed: %v", err)
	}

	presence, err := store.Get(ctx, "u1001")
	if err != nil {
		t.Fatalf("get session presence failed: %v", err)
	}
	if presence == nil {
		t.Fatal("expected session presence")
	}
	if presence.NodeID != "sig-node-a" || presence.SessionID != 101 || presence.MeetingID != "m1001" || presence.Status != 2 {
		t.Fatalf("unexpected session presence: %+v", presence)
	}

	if err := store.Delete(ctx, "u1001", 999); err != nil {
		t.Fatalf("delete mismatched session failed: %v", err)
	}
	presence, err = store.Get(ctx, "u1001")
	if err != nil || presence == nil {
		t.Fatalf("expected session presence to remain after mismatched delete, presence=%+v err=%v", presence, err)
	}

	if err := store.Delete(ctx, "u1001", 101); err != nil {
		t.Fatalf("delete matching session failed: %v", err)
	}
	presence, err = store.Get(ctx, "u1001")
	if err != nil {
		t.Fatalf("get after delete failed: %v", err)
	}
	if presence != nil {
		t.Fatalf("expected deleted session presence, got %+v", presence)
	}
}
