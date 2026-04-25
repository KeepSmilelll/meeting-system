package store

import (
	"context"
	"math"
	"testing"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"

	"github.com/alicebob/miniredis/v2"
)

func TestRedisRoomStoreMuteAllPersistsAndClearsOnDelete(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.UpsertRoom(ctx, "m1001", "u1001", "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}

	muted, err := roomStore.MuteAll(ctx, "m1001")
	if err != nil {
		t.Fatalf("load initial mute_all failed: %v", err)
	}
	if muted {
		t.Fatal("expected initial mute_all=false")
	}

	if err := roomStore.SetMuteAll(ctx, "m1001", true); err != nil {
		t.Fatalf("set mute_all failed: %v", err)
	}

	muted, err = roomStore.MuteAll(ctx, "m1001")
	if err != nil {
		t.Fatalf("load persisted mute_all failed: %v", err)
	}
	if !muted {
		t.Fatal("expected mute_all=true after update")
	}

	if err := roomStore.DeleteRoom(ctx, "m1001"); err != nil {
		t.Fatalf("delete room failed: %v", err)
	}

	muted, err = roomStore.MuteAll(ctx, "m1001")
	if err != nil {
		t.Fatalf("load mute_all after delete failed: %v", err)
	}
	if muted {
		t.Fatal("expected mute_all=false after room deletion")
	}
}

func TestRedisRoomStoreHydratesParticipantMediaState(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	participant := &protocol.Participant{
		UserId:      "u1002",
		DisplayName: "Alice",
		IsAudioOn:   true,
		IsVideoOn:   true,
		IsSharing:   false,
	}
	if err := roomStore.AddMember(ctx, "m2001", participant); err != nil {
		t.Fatalf("add member failed: %v", err)
	}
	if err := roomStore.SetParticipantMediaMuted(ctx, "m2001", "u1002", 0, true); err != nil {
		t.Fatalf("set audio muted failed: %v", err)
	}
	if err := roomStore.SetParticipantMediaMuted(ctx, "m2001", "u1002", 1, true); err != nil {
		t.Fatalf("set video muted failed: %v", err)
	}
	if err := roomStore.SetParticipantSharing(ctx, "m2001", "u1002", true); err != nil {
		t.Fatalf("set sharing failed: %v", err)
	}
	if err := roomStore.SetParticipantMediaSsrc(ctx, "m2001", "u1002", 1111, 2222); err != nil {
		t.Fatalf("set participant SSRCs failed: %v", err)
	}

	hydrated := roomStore.HydrateParticipants(ctx, "m2001", []*protocol.Participant{participant})
	if len(hydrated) != 1 || hydrated[0] == nil {
		t.Fatalf("expected one hydrated participant, got %+v", hydrated)
	}
	if hydrated[0].IsAudioOn || hydrated[0].IsVideoOn || !hydrated[0].IsSharing {
		t.Fatalf("unexpected hydrated participant state: %+v", hydrated[0])
	}
	if hydrated[0].AudioSsrc != 1111 || hydrated[0].VideoSsrc != 2222 {
		t.Fatalf("unexpected hydrated participant SSRCs: %+v", hydrated[0])
	}
	if !participant.IsAudioOn || !participant.IsVideoOn || participant.IsSharing {
		t.Fatalf("expected source participant to remain unchanged, got %+v", participant)
	}
	if participant.AudioSsrc != 0 || participant.VideoSsrc != 0 {
		t.Fatalf("expected source participant SSRCs to remain unchanged, got %+v", participant)
	}
}

func TestRedisRoomStoreRoomMetadataRoundTrip(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.UpsertRoom(ctx, "m3001", "u9001", "sfu-node-02", "10.0.0.2:10000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomStore.SetMuteAll(ctx, "m3001", true); err != nil {
		t.Fatalf("set mute_all failed: %v", err)
	}

	meta, err := roomStore.RoomMetadata(ctx, "m3001")
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil {
		t.Fatal("expected room metadata")
	}
	if meta.HostUserID != "u9001" || meta.SFUNodeID != "sfu-node-02" || meta.SFUAddress != "10.0.0.2:10000" || !meta.MuteAll {
		t.Fatalf("unexpected room metadata: %+v", meta)
	}

	if err := roomStore.DeleteRoom(ctx, "m3001"); err != nil {
		t.Fatalf("delete room failed: %v", err)
	}
	meta, err = roomStore.RoomMetadata(ctx, "m3001")
	if err != nil {
		t.Fatalf("load room metadata after delete failed: %v", err)
	}
	if meta != nil {
		t.Fatalf("expected no room metadata after delete, got %+v", meta)
	}
}

func TestRedisRoomStoreSwitchRoomRoutePreservesRoomState(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.UpsertRoom(ctx, "m-switch", "u1001", "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomStore.SetMuteAll(ctx, "m-switch", true); err != nil {
		t.Fatalf("set mute_all failed: %v", err)
	}

	switched, err := roomStore.SwitchRoomRoute(ctx, "m-switch", "sfu-a", "sfu-b", "10.0.0.3:10000")
	if err != nil {
		t.Fatalf("switch room route failed: %v", err)
	}
	if !switched {
		t.Fatal("expected room route switch to succeed")
	}

	meta, err := roomStore.RoomMetadata(ctx, "m-switch")
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil {
		t.Fatal("expected room metadata")
	}
	if meta.HostUserID != "u1001" || meta.SFUNodeID != "sfu-b" || meta.SFUAddress != "10.0.0.3:10000" || !meta.MuteAll {
		t.Fatalf("unexpected metadata after route switch: %+v", meta)
	}
}

func TestRedisRoomStoreSwitchRoomRouteRejectsStaleExpectedNode(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.UpsertRoom(ctx, "m-switch-stale", "u2001", "sfu-b", "10.0.0.3:10000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomStore.SetMuteAll(ctx, "m-switch-stale", true); err != nil {
		t.Fatalf("set mute_all failed: %v", err)
	}

	switched, err := roomStore.SwitchRoomRoute(ctx, "m-switch-stale", "sfu-a", "sfu-c", "10.0.0.4:10000")
	if err != nil {
		t.Fatalf("switch room route with stale expected node failed: %v", err)
	}
	if switched {
		t.Fatal("expected room route switch to be rejected on stale expected node")
	}

	meta, err := roomStore.RoomMetadata(ctx, "m-switch-stale")
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil {
		t.Fatal("expected room metadata")
	}
	if meta.HostUserID != "u2001" || meta.SFUNodeID != "sfu-b" || meta.SFUAddress != "10.0.0.3:10000" || !meta.MuteAll {
		t.Fatalf("room metadata should remain unchanged on rejected switch: %+v", meta)
	}
}

func TestRedisRoomStoreNextSFUNodeUsesRedisRoundRobin(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	nodes := []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000"},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000"},
		{NodeID: "sfu-c", MediaAddress: "media-c:10000", RPCAddress: "rpc-c:9000"},
	}

	first, err := roomStore.NextSFUNode(context.Background(), nodes)
	if err != nil {
		t.Fatalf("select first sfu node failed: %v", err)
	}
	second, err := roomStore.NextSFUNode(context.Background(), nodes)
	if err != nil {
		t.Fatalf("select second sfu node failed: %v", err)
	}
	third, err := roomStore.NextSFUNode(context.Background(), nodes)
	if err != nil {
		t.Fatalf("select third sfu node failed: %v", err)
	}
	fourth, err := roomStore.NextSFUNode(context.Background(), nodes)
	if err != nil {
		t.Fatalf("select fourth sfu node failed: %v", err)
	}

	if first.NodeID != "sfu-a" || second.NodeID != "sfu-b" || third.NodeID != "sfu-c" || fourth.NodeID != "sfu-a" {
		t.Fatalf("unexpected redis round-robin selection: first=%+v second=%+v third=%+v fourth=%+v", first, second, third, fourth)
	}
}

func TestRedisRoomStoreRankedSFUNodesPrefersHealthyCapacity(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()
	nodes := []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000", MaxMeetings: 1},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000", MaxMeetings: 2},
		{NodeID: "sfu-c", MediaAddress: "media-c:10000", RPCAddress: "rpc-c:9000", MaxMeetings: 1},
	}

	if err := roomStore.UpsertRoom(ctx, "m4001", "u1001", "sfu-a", "media-a:10000"); err != nil {
		t.Fatalf("upsert sfu-a room failed: %v", err)
	}
	if err := roomStore.MarkSFUNodeFailure(ctx, "sfu-c", time.Minute); err != nil {
		t.Fatalf("mark sfu-c failure failed: %v", err)
	}

	ranked, err := roomStore.RankedSFUNodes(ctx, nodes)
	if err != nil {
		t.Fatalf("rank sfu nodes failed: %v", err)
	}
	if len(ranked) != 3 {
		t.Fatalf("expected three ranked nodes, got %+v", ranked)
	}
	if ranked[0].NodeID != "sfu-b" || ranked[1].NodeID != "sfu-a" || ranked[2].NodeID != "sfu-c" {
		t.Fatalf("unexpected ranked order: %+v", ranked)
	}
}

func TestRedisRoomStoreReportSFUNodeStatusPersistsRuntimeAndLoad(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.ReportSFUNodeStatus(ctx, "sfu-a", SFUNodeStatus{
		SFUAddress:     "media-a:10000",
		MediaPort:      5004,
		RoomCount:      4,
		PublisherCount: 9,
		PacketCount:    123,
		UpdatedAtUnix:  1710000000,
	}, 30*time.Second); err != nil {
		t.Fatalf("report node status failed: %v", err)
	}

	status, err := roomStore.SFUNodeStatus(ctx, "sfu-a")
	if err != nil {
		t.Fatalf("load node status failed: %v", err)
	}
	if status == nil {
		t.Fatal("expected node status")
	}
	if status.SFUAddress != "media-a:10000" || status.MediaPort != 5004 || status.RoomCount != 4 || status.PublisherCount != 9 || status.PacketCount != 123 {
		t.Fatalf("unexpected node status: %+v", status)
	}

	ranked, err := roomStore.RankedSFUNodes(ctx, []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000", MaxMeetings: 10},
	})
	if err != nil {
		t.Fatalf("rank nodes failed: %v", err)
	}
	if len(ranked) != 2 || ranked[0].NodeID != "sfu-b" || ranked[1].NodeID != "sfu-a" {
		t.Fatalf("expected reported room_count to influence load ordering, got %+v", ranked)
	}
}
func TestRedisRoomStoreReportParticipantQualityRoundTrip(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.AddMember(ctx, "m5001", &protocol.Participant{UserId: "u5001", DisplayName: "Alice"}); err != nil {
		t.Fatalf("add member failed: %v", err)
	}
	if err := roomStore.ReportParticipantQuality(ctx, "m5001", "u5001", ParticipantQuality{
		PacketLoss:    0.125,
		RttMs:         88,
		JitterMs:      12,
		BitrateKbps:   640,
		UpdatedAtUnix: 1710000001,
	}); err != nil {
		t.Fatalf("report participant quality failed: %v", err)
	}

	quality, err := roomStore.LoadParticipantQuality(ctx, "m5001", "u5001")
	if err != nil {
		t.Fatalf("load participant quality failed: %v", err)
	}
	if quality == nil {
		t.Fatal("expected participant quality")
	}
	if math.Abs(float64(quality.PacketLoss-0.125)) > 0.0001 || quality.RttMs != 88 || quality.JitterMs != 12 || quality.BitrateKbps != 640 || quality.UpdatedAtUnix != 1710000001 {
		t.Fatalf("unexpected participant quality: %+v", quality)
	}
}

func TestRedisRoomStoreMeetingRecoveryLockAcquireRelease(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	acquired, err := roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock", "node-a:owner-1", 10*time.Second)
	if err != nil {
		t.Fatalf("acquire lock first owner failed: %v", err)
	}
	if !acquired {
		t.Fatal("expected first owner lock acquisition to succeed")
	}

	acquired, err = roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock", "node-b:owner-1", 10*time.Second)
	if err != nil {
		t.Fatalf("acquire lock second owner failed: %v", err)
	}
	if acquired {
		t.Fatal("expected second owner lock acquisition to fail while lock is held")
	}

	if err := roomStore.ReleaseMeetingRecoveryLock(ctx, "m-lock", "node-b:owner-1"); err != nil {
		t.Fatalf("release lock with wrong owner failed: %v", err)
	}

	acquired, err = roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock", "node-c:owner-1", 10*time.Second)
	if err != nil {
		t.Fatalf("acquire lock third owner failed: %v", err)
	}
	if acquired {
		t.Fatal("expected third owner lock acquisition to fail before correct owner release")
	}

	if err := roomStore.ReleaseMeetingRecoveryLock(ctx, "m-lock", "node-a:owner-1"); err != nil {
		t.Fatalf("release lock with correct owner failed: %v", err)
	}

	acquired, err = roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock", "node-b:owner-1", 10*time.Second)
	if err != nil {
		t.Fatalf("acquire lock after release failed: %v", err)
	}
	if !acquired {
		t.Fatal("expected lock acquisition to succeed after release")
	}
}

func TestRedisRoomStoreMeetingRecoveryLockExpires(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	acquired, err := roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock-expire", "node-a:owner-1", 2*time.Second)
	if err != nil {
		t.Fatalf("acquire expiring lock failed: %v", err)
	}
	if !acquired {
		t.Fatal("expected initial lock acquisition to succeed")
	}

	mini.FastForward(3 * time.Second)

	acquired, err = roomStore.AcquireMeetingRecoveryLock(ctx, "m-lock-expire", "node-b:owner-2", 2*time.Second)
	if err != nil {
		t.Fatalf("acquire lock after ttl expiration failed: %v", err)
	}
	if !acquired {
		t.Fatal("expected lock acquisition to succeed after ttl expiration")
	}
}

func TestRedisRoomStoreRecoveryMetricsRoundTrip(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	sequence := []string{
		"lock_attempts",
		"lock_acquired",
		"lock_attempts",
		"lock_contended",
		"followup_attempts",
		"followup_success",
		"failover_attempts",
		"failover_failed",
		"route_status_emitted",
		"route_status_deduped",
	}
	for _, field := range sequence {
		if err := roomStore.IncrementRecoveryMetric(ctx, "sig-node-a", field); err != nil {
			t.Fatalf("increment recovery metric %q failed: %v", field, err)
		}
	}

	metrics, err := roomStore.LoadRecoveryMetrics(ctx, "sig-node-a")
	if err != nil {
		t.Fatalf("load recovery metrics failed: %v", err)
	}
	if metrics == nil {
		t.Fatal("expected recovery metrics")
	}
	if metrics.LockAttempts != 2 || metrics.LockAcquired != 1 || metrics.LockContended != 1 {
		t.Fatalf("unexpected lock metrics: %+v", metrics)
	}
	if metrics.FollowupAttempts != 1 || metrics.FollowupSuccess != 1 || metrics.FollowupFailed != 0 {
		t.Fatalf("unexpected followup metrics: %+v", metrics)
	}
	if metrics.FailoverAttempts != 1 || metrics.FailoverFailed != 1 || metrics.FailoverSuccess != 0 {
		t.Fatalf("unexpected failover metrics: %+v", metrics)
	}
	if metrics.RouteStatusSent != 1 || metrics.RouteStatusDrop != 1 {
		t.Fatalf("unexpected route status metrics: %+v", metrics)
	}
	if metrics.UpdatedAtUnix <= 0 {
		t.Fatalf("expected updated_at to be populated, got %+v", metrics)
	}
}

func TestRedisRoomStoreSFUNodeStatusIncludesRecoveryMetrics(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.ReportSFUNodeStatus(ctx, "sfu-metric", SFUNodeStatus{
		SFUAddress:     "10.0.0.9:10000",
		MediaPort:      10000,
		RoomCount:      4,
		PublisherCount: 12,
		PacketCount:    999,
		UpdatedAtUnix:  1710000100,
	}, 30*time.Second); err != nil {
		t.Fatalf("report sfu status failed: %v", err)
	}

	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-metric", "attempts"); err != nil {
		t.Fatalf("increment attempts failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-metric", "failover_success"); err != nil {
		t.Fatalf("increment failover_success failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-metric", "failover_failed"); err != nil {
		t.Fatalf("increment failover_failed failed: %v", err)
	}

	status, err := roomStore.SFUNodeStatus(ctx, "sfu-metric")
	if err != nil {
		t.Fatalf("load sfu status with metrics failed: %v", err)
	}
	if status == nil {
		t.Fatal("expected sfu status with recovery metrics")
	}
	if status.RoomCount != 4 || status.PublisherCount != 12 || status.PacketCount != 999 {
		t.Fatalf("unexpected runtime snapshot: %+v", status)
	}
	if status.RecoveryAttempts != 1 || status.RecoveryFailoverSuccess != 1 || status.RecoveryFailoverFailed != 1 {
		t.Fatalf("unexpected recovery metrics snapshot: %+v", status)
	}
	if status.RecoveryUpdatedAtUnix <= 0 {
		t.Fatalf("expected recovery updated_at to be populated, got %+v", status)
	}
}

func TestRedisRoomStoreSFUNodeStatusReturnsRecoveryOnlySnapshot(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-recovery-only", "attempts"); err != nil {
		t.Fatalf("increment attempts failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-recovery-only", "failover_failed"); err != nil {
		t.Fatalf("increment failover_failed failed: %v", err)
	}

	status, err := roomStore.SFUNodeStatus(ctx, "sfu-recovery-only")
	if err != nil {
		t.Fatalf("load recovery-only sfu status failed: %v", err)
	}
	if status == nil {
		t.Fatal("expected recovery-only sfu snapshot")
	}
	if status.SFUAddress != "" || status.RoomCount != 0 || status.PublisherCount != 0 {
		t.Fatalf("expected zero runtime fields for recovery-only snapshot, got %+v", status)
	}
	if status.RecoveryAttempts != 1 || status.RecoveryFailoverFailed != 1 || status.RecoveryFailoverSuccess != 0 {
		t.Fatalf("unexpected recovery-only metrics snapshot: %+v", status)
	}
}

func TestRedisRoomStoreRecoveryMetricNodeIDs(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	roomStore := NewRedisRoomStoreWithClient(client)
	ctx := context.Background()

	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-a", "attempts"); err != nil {
		t.Fatalf("increment sfu-a attempts failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-b", "failover_failed"); err != nil {
		t.Fatalf("increment sfu-b failover_failed failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-a", "failover_success"); err != nil {
		t.Fatalf("increment sfu-a failover_success failed: %v", err)
	}

	nodeIDs, err := roomStore.RecoveryMetricNodeIDs(ctx)
	if err != nil {
		t.Fatalf("load recovery metric node ids failed: %v", err)
	}
	if len(nodeIDs) != 2 || nodeIDs[0] != "sfu-a" || nodeIDs[1] != "sfu-b" {
		t.Fatalf("unexpected recovery metric node ids: %+v", nodeIDs)
	}
}
