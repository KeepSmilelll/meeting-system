package sfu

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/store"
)

type AdminServer struct {
	cfg       config.Config
	roomState *store.RedisRoomStore

	mu       sync.Mutex
	listener net.Listener
	server   *http.Server
}

type sfuRuntimeSnapshot struct {
	SFUAddress     string `json:"sfu_address"`
	MediaPort      uint32 `json:"media_port"`
	RoomCount      int    `json:"room_count"`
	PublisherCount int    `json:"publisher_count"`
	PacketCount    uint64 `json:"packet_count"`
	UpdatedAtUnix  int64  `json:"updated_at_unix"`
}

type sfuRecoverySnapshot struct {
	Attempts       int64 `json:"attempts"`
	FailoverOK     int64 `json:"failover_success"`
	FailoverFailed int64 `json:"failover_failed"`
	UpdatedAtUnix  int64 `json:"updated_at_unix"`
}

type sfuNodeSnapshot struct {
	NodeID       string              `json:"node_id"`
	MediaAddress string              `json:"media_address,omitempty"`
	RPCAddress   string              `json:"rpc_address,omitempty"`
	MaxMeetings  int                 `json:"max_meetings"`
	Runtime      sfuRuntimeSnapshot  `json:"runtime"`
	Recovery     sfuRecoverySnapshot `json:"recovery"`
}

type sfuNodeSnapshotResponse struct {
	GeneratedAtUnix   int64                      `json:"generated_at_unix"`
	SignalingNodeID   string                     `json:"signaling_node_id,omitempty"`
	SignalingRecovery *signalingRecoverySnapshot `json:"signaling_recovery,omitempty"`
	Nodes             []sfuNodeSnapshot          `json:"nodes"`
}

type signalingRecoverySnapshot struct {
	LockAttempts     int64 `json:"lock_attempts"`
	LockAcquired     int64 `json:"lock_acquired"`
	LockContended    int64 `json:"lock_contended"`
	FollowupAttempts int64 `json:"followup_attempts"`
	FollowupSuccess  int64 `json:"followup_success"`
	FollowupFailed   int64 `json:"followup_failed"`
	FailoverAttempts int64 `json:"failover_attempts"`
	FailoverSuccess  int64 `json:"failover_success"`
	FailoverFailed   int64 `json:"failover_failed"`
	RouteStatusSent  int64 `json:"route_status_emitted"`
	RouteStatusDrop  int64 `json:"route_status_deduped"`
	UpdatedAtUnix    int64 `json:"updated_at_unix"`
}

func NewAdminServer(cfg config.Config, roomState *store.RedisRoomStore) *AdminServer {
	return &AdminServer{
		cfg:       cfg,
		roomState: roomState,
	}
}

func (s *AdminServer) Start(ctx context.Context) error {
	if s == nil || s.roomState == nil || !s.roomState.Enabled() || s.cfg.AdminListenAddr == "" {
		return nil
	}

	listener, err := net.Listen("tcp", s.cfg.AdminListenAddr)
	if err != nil {
		return fmt.Errorf("admin server listen %s: %w", s.cfg.AdminListenAddr, err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/admin/sfu/nodes", s.handleSFUNodes)

	server := &http.Server{Handler: mux}

	s.mu.Lock()
	s.listener = listener
	s.server = server
	s.mu.Unlock()

	go func() {
		<-ctx.Done()
		s.close()
	}()

	go func() {
		_ = server.Serve(listener)
	}()
	return nil
}

func (s *AdminServer) close() {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.server != nil {
		shutdownCtx, cancel := context.WithTimeout(context.Background(), time.Second)
		_ = s.server.Shutdown(shutdownCtx)
		cancel()
		s.server = nil
	}
	if s.listener != nil {
		_ = s.listener.Close()
		s.listener = nil
	}
}

func (s *AdminServer) handleSFUNodes(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if s == nil || s.roomState == nil {
		http.Error(w, "service unavailable", http.StatusServiceUnavailable)
		return
	}

	nodeFilter := parseNodeFilter(r)
	includeSignaling := parseIncludeSignaling(r)
	signalingRecovery, snapshots, err := s.collectSFUNodes(r.Context(), nodeFilter, includeSignaling)
	if err != nil {
		http.Error(w, "failed to collect sfu node snapshots", http.StatusInternalServerError)
		return
	}

	signalingNodeID := ""
	if includeSignaling {
		signalingNodeID = s.cfg.NodeID
	}

	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(sfuNodeSnapshotResponse{
		GeneratedAtUnix:   time.Now().Unix(),
		SignalingNodeID:   signalingNodeID,
		SignalingRecovery: signalingRecovery,
		Nodes:             snapshots,
	})
}

func (s *AdminServer) collectSFUNodes(ctx context.Context, filter map[string]struct{}, includeSignaling bool) (*signalingRecoverySnapshot, []sfuNodeSnapshot, error) {
	var signalingRecovery *signalingRecoverySnapshot
	if includeSignaling && s.cfg.NodeID != "" {
		snapshot := signalingRecoverySnapshot{}
		metrics, err := s.roomState.LoadRecoveryMetrics(ctx, s.cfg.NodeID)
		if err != nil {
			return nil, nil, err
		}
		if metrics != nil {
			snapshot = signalingRecoverySnapshot{
				LockAttempts:     metrics.LockAttempts,
				LockAcquired:     metrics.LockAcquired,
				LockContended:    metrics.LockContended,
				FollowupAttempts: metrics.FollowupAttempts,
				FollowupSuccess:  metrics.FollowupSuccess,
				FollowupFailed:   metrics.FollowupFailed,
				FailoverAttempts: metrics.FailoverAttempts,
				FailoverSuccess:  metrics.FailoverSuccess,
				FailoverFailed:   metrics.FailoverFailed,
				RouteStatusSent:  metrics.RouteStatusSent,
				RouteStatusDrop:  metrics.RouteStatusDrop,
				UpdatedAtUnix:    metrics.UpdatedAtUnix,
			}
		}
		signalingRecovery = &snapshot
	}

	merged := make(map[string]config.SFUNode)

	for _, node := range s.cfg.EffectiveSFUNodes() {
		if node.NodeID == "" {
			continue
		}
		merged[node.NodeID] = node
	}

	registered, err := s.roomState.RegisteredSFUNodes(ctx)
	if err != nil {
		return nil, nil, err
	}
	for _, node := range registered {
		if node.NodeID == "" {
			continue
		}
		merged[node.NodeID] = node
	}

	recoveryNodeIDs, err := s.roomState.RecoveryMetricNodeIDs(ctx)
	if err != nil {
		return nil, nil, err
	}
	for _, nodeID := range recoveryNodeIDs {
		if nodeID == "" {
			continue
		}
		if _, ok := merged[nodeID]; !ok {
			merged[nodeID] = config.SFUNode{NodeID: nodeID}
		}
	}

	nodeIDs := make([]string, 0, len(merged))
	for nodeID := range merged {
		nodeIDs = append(nodeIDs, nodeID)
	}
	sort.Strings(nodeIDs)

	out := make([]sfuNodeSnapshot, 0, len(nodeIDs))
	for _, nodeID := range nodeIDs {
		if len(filter) > 0 {
			if _, ok := filter[nodeID]; !ok {
				continue
			}
		}
		node := merged[nodeID]
		status, err := s.roomState.SFUNodeStatus(ctx, nodeID)
		if err != nil {
			return nil, nil, err
		}

		snapshot := sfuNodeSnapshot{
			NodeID:       nodeID,
			MediaAddress: node.MediaAddress,
			RPCAddress:   node.RPCAddress,
			MaxMeetings:  node.MaxMeetings,
		}
		if status != nil {
			snapshot.Runtime = sfuRuntimeSnapshot{
				SFUAddress:     status.SFUAddress,
				MediaPort:      status.MediaPort,
				RoomCount:      status.RoomCount,
				PublisherCount: status.PublisherCount,
				PacketCount:    status.PacketCount,
				UpdatedAtUnix:  status.UpdatedAtUnix,
			}
			snapshot.Recovery = sfuRecoverySnapshot{
				Attempts:       status.RecoveryAttempts,
				FailoverOK:     status.RecoveryFailoverSuccess,
				FailoverFailed: status.RecoveryFailoverFailed,
				UpdatedAtUnix:  status.RecoveryUpdatedAtUnix,
			}
		}
		out = append(out, snapshot)
	}

	return signalingRecovery, out, nil
}

func parseIncludeSignaling(r *http.Request) bool {
	if r == nil {
		return true
	}
	raw := strings.TrimSpace(r.URL.Query().Get("include_signaling"))
	if raw == "" {
		return true
	}
	parsed, err := strconv.ParseBool(raw)
	if err != nil {
		return true
	}
	return parsed
}

func parseNodeFilter(r *http.Request) map[string]struct{} {
	if r == nil {
		return nil
	}

	values := r.URL.Query()["node_id"]
	if len(values) == 0 {
		return nil
	}

	filter := make(map[string]struct{})
	for _, raw := range values {
		for _, part := range strings.Split(raw, ",") {
			nodeID := strings.TrimSpace(part)
			if nodeID == "" {
				continue
			}
			filter[nodeID] = struct{}{}
		}
	}
	if len(filter) == 0 {
		return nil
	}
	return filter
}
