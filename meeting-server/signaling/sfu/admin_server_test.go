package sfu

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"testing"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/store"

	"github.com/alicebob/miniredis/v2"
)

func waitForHTTPReady(url string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		resp, err := http.Get(url) // #nosec G107 -- test-only local loopback server.
		if err == nil {
			_ = resp.Body.Close()
			return nil
		}
		time.Sleep(20 * time.Millisecond)
	}
	return fmt.Errorf("http endpoint %s not ready before timeout", url)
}

func reserveAdminLoopbackAddr(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve admin loopback addr failed: %v", err)
	}
	addr := listener.Addr().String()
	_ = listener.Close()
	return addr
}

func TestAdminServerExposesSFUSnapshotsWithRecoveryMetrics(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.AdminListenAddr = reserveAdminLoopbackAddr(t)
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 16},
	}

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()
	ctx := context.Background()

	if err := roomStore.ReportSFUNodeStatus(ctx, "sfu-a", store.SFUNodeStatus{
		SFUAddress:     "10.0.0.2:10000",
		MediaPort:      10000,
		RoomCount:      3,
		PublisherCount: 9,
		PacketCount:    1200,
		UpdatedAtUnix:  1710000200,
	}, 30*time.Second); err != nil {
		t.Fatalf("report sfu-a status failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-a", "attempts"); err != nil {
		t.Fatalf("increment sfu-a recovery attempts failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-a", "failover_success"); err != nil {
		t.Fatalf("increment sfu-a recovery success failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-recovery-only", "failover_failed"); err != nil {
		t.Fatalf("increment recovery-only failed metric failed: %v", err)
	}
	if err := roomStore.IncrementRecoveryMetric(ctx, cfg.NodeID, "lock_attempts"); err != nil {
		t.Fatalf("increment signaling lock_attempts failed: %v", err)
	}
	if err := roomStore.IncrementRecoveryMetric(ctx, cfg.NodeID, "lock_contended"); err != nil {
		t.Fatalf("increment signaling lock_contended failed: %v", err)
	}
	if err := roomStore.IncrementRecoveryMetric(ctx, cfg.NodeID, "route_status_emitted"); err != nil {
		t.Fatalf("increment signaling route_status_emitted failed: %v", err)
	}
	if err := roomStore.IncrementRecoveryMetric(ctx, cfg.NodeID, "route_status_deduped"); err != nil {
		t.Fatalf("increment signaling route_status_deduped failed: %v", err)
	}

	serverCtx, cancel := context.WithCancel(context.Background())
	defer cancel()

	server := NewAdminServer(cfg, roomStore)
	if err := server.Start(serverCtx); err != nil {
		t.Fatalf("start admin server failed: %v", err)
	}

	url := "http://" + cfg.AdminListenAddr + "/admin/sfu/nodes"
	if err := waitForHTTPReady(url, 2*time.Second); err != nil {
		t.Fatalf("wait for admin server readiness failed: %v", err)
	}

	resp, err := http.Get(url) // #nosec G107 -- test-only local loopback server.
	if err != nil {
		t.Fatalf("get sfu snapshot failed: %v", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("unexpected status code: got %d want %d", resp.StatusCode, http.StatusOK)
	}

	var body sfuNodeSnapshotResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode sfu snapshot response failed: %v", err)
	}
	if body.GeneratedAtUnix <= 0 {
		t.Fatalf("expected generated_at_unix > 0, got %d", body.GeneratedAtUnix)
	}
	if body.SignalingNodeID != cfg.NodeID {
		t.Fatalf("unexpected signaling node id: got %q want %q", body.SignalingNodeID, cfg.NodeID)
	}
	if body.SignalingRecovery == nil {
		t.Fatalf("expected signaling recovery snapshot")
	}
	if body.SignalingRecovery.LockAttempts != 1 || body.SignalingRecovery.LockContended != 1 || body.SignalingRecovery.LockAcquired != 0 {
		t.Fatalf("unexpected signaling recovery snapshot: %+v", body.SignalingRecovery)
	}
	if body.SignalingRecovery.RouteStatusSent != 1 || body.SignalingRecovery.RouteStatusDrop != 1 {
		t.Fatalf("unexpected signaling route status metrics: %+v", body.SignalingRecovery)
	}
	if len(body.Nodes) != 2 {
		t.Fatalf("expected two nodes in snapshot (configured + recovery-only), got %+v", body.Nodes)
	}

	var snapshotA, recoveryOnly *sfuNodeSnapshot
	for i := range body.Nodes {
		node := &body.Nodes[i]
		switch node.NodeID {
		case "sfu-a":
			snapshotA = node
		case "sfu-recovery-only":
			recoveryOnly = node
		}
	}
	if snapshotA == nil {
		t.Fatalf("expected snapshot for sfu-a, got %+v", body.Nodes)
	}
	if snapshotA.Runtime.RoomCount != 3 || snapshotA.Runtime.PublisherCount != 9 || snapshotA.Recovery.Attempts != 1 || snapshotA.Recovery.FailoverOK != 1 {
		t.Fatalf("unexpected sfu-a snapshot payload: %+v", snapshotA)
	}

	if recoveryOnly == nil {
		t.Fatalf("expected recovery-only node snapshot, got %+v", body.Nodes)
	}
	if recoveryOnly.Runtime.RoomCount != 0 || recoveryOnly.Recovery.FailoverFailed != 1 || recoveryOnly.Recovery.Attempts != 0 {
		t.Fatalf("unexpected recovery-only snapshot payload: %+v", recoveryOnly)
	}
}

func TestAdminServerSupportsNodeIDFilter(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.AdminListenAddr = reserveAdminLoopbackAddr(t)
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 16},
	}

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()
	ctx := context.Background()

	if err := roomStore.ReportSFUNodeStatus(ctx, "sfu-a", store.SFUNodeStatus{
		SFUAddress:     "10.0.0.2:10000",
		MediaPort:      10000,
		RoomCount:      3,
		PublisherCount: 9,
		PacketCount:    1200,
		UpdatedAtUnix:  1710000200,
	}, 30*time.Second); err != nil {
		t.Fatalf("report sfu-a status failed: %v", err)
	}
	if err := roomStore.IncrementSFUNodeRecoveryMetric(ctx, "sfu-recovery-only", "failover_failed"); err != nil {
		t.Fatalf("increment recovery-only failed metric failed: %v", err)
	}

	serverCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	server := NewAdminServer(cfg, roomStore)
	if err := server.Start(serverCtx); err != nil {
		t.Fatalf("start admin server failed: %v", err)
	}

	baseURL := "http://" + cfg.AdminListenAddr + "/admin/sfu/nodes"
	if err := waitForHTTPReady(baseURL, 2*time.Second); err != nil {
		t.Fatalf("wait for admin server readiness failed: %v", err)
	}

	resp, err := http.Get(baseURL + "?node_id=sfu-a") // #nosec G107 -- test-only local loopback server.
	if err != nil {
		t.Fatalf("get filtered sfu snapshot failed: %v", err)
	}
	defer resp.Body.Close()

	var single sfuNodeSnapshotResponse
	if err := json.NewDecoder(resp.Body).Decode(&single); err != nil {
		t.Fatalf("decode filtered sfu snapshot failed: %v", err)
	}
	if len(single.Nodes) != 1 || single.Nodes[0].NodeID != "sfu-a" {
		t.Fatalf("unexpected node filter result: %+v", single.Nodes)
	}

	resp2, err := http.Get(baseURL + "?node_id=sfu-a,sfu-recovery-only") // #nosec G107 -- test-only local loopback server.
	if err != nil {
		t.Fatalf("get multi-node filtered snapshot failed: %v", err)
	}
	defer resp2.Body.Close()

	var multi sfuNodeSnapshotResponse
	if err := json.NewDecoder(resp2.Body).Decode(&multi); err != nil {
		t.Fatalf("decode multi-node filtered snapshot failed: %v", err)
	}
	if len(multi.Nodes) != 2 {
		t.Fatalf("expected two filtered nodes, got %+v", multi.Nodes)
	}
	if multi.Nodes[0].NodeID != "sfu-a" || multi.Nodes[1].NodeID != "sfu-recovery-only" {
		t.Fatalf("unexpected multi-node filter order/content: %+v", multi.Nodes)
	}
}

func TestAdminServerCanSkipSignalingSnapshot(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.AdminListenAddr = reserveAdminLoopbackAddr(t)
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 16},
	}

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()
	ctx := context.Background()

	if err := roomStore.ReportSFUNodeStatus(ctx, "sfu-a", store.SFUNodeStatus{
		SFUAddress:     "10.0.0.2:10000",
		MediaPort:      10000,
		RoomCount:      3,
		PublisherCount: 9,
		PacketCount:    1200,
		UpdatedAtUnix:  1710000200,
	}, 30*time.Second); err != nil {
		t.Fatalf("report sfu-a status failed: %v", err)
	}
	if err := roomStore.IncrementRecoveryMetric(ctx, cfg.NodeID, "lock_attempts"); err != nil {
		t.Fatalf("increment signaling metric failed: %v", err)
	}

	serverCtx, cancel := context.WithCancel(context.Background())
	defer cancel()
	server := NewAdminServer(cfg, roomStore)
	if err := server.Start(serverCtx); err != nil {
		t.Fatalf("start admin server failed: %v", err)
	}

	baseURL := "http://" + cfg.AdminListenAddr + "/admin/sfu/nodes"
	if err := waitForHTTPReady(baseURL, 2*time.Second); err != nil {
		t.Fatalf("wait for admin server readiness failed: %v", err)
	}

	resp, err := http.Get(baseURL + "?include_signaling=false") // #nosec G107 -- test-only local loopback server.
	if err != nil {
		t.Fatalf("get snapshot without signaling failed: %v", err)
	}
	defer resp.Body.Close()

	var body sfuNodeSnapshotResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode snapshot without signaling failed: %v", err)
	}
	if body.SignalingNodeID != "" {
		t.Fatalf("expected empty signaling_node_id when include_signaling=false, got %q", body.SignalingNodeID)
	}
	if body.SignalingRecovery != nil {
		t.Fatalf("expected signaling_recovery to be omitted when include_signaling=false, got %+v", body.SignalingRecovery)
	}
	if len(body.Nodes) != 1 || body.Nodes[0].NodeID != "sfu-a" {
		t.Fatalf("unexpected nodes payload when include_signaling=false: %+v", body.Nodes)
	}
}
