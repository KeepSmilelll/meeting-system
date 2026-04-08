package sfu

import (
	"context"
	"sync"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/store"
)

type NodeStatusMonitor struct {
	cfg       config.Config
	roomState *store.RedisRoomStore

	clientsMu sync.RWMutex
	clients   map[string]Client
	options   []Option
}

func NewNodeStatusMonitor(cfg config.Config, roomState *store.RedisRoomStore, options ...Option) *NodeStatusMonitor {
	clients := make(map[string]Client)
	for _, node := range cfg.EffectiveSFUNodes() {
		if node.NodeID == "" || node.RPCAddress == "" {
			continue
		}
		clients[node.NodeID] = NewClient(node.RPCAddress, options...)
	}
	monitor := NewNodeStatusMonitorWithClients(cfg, roomState, clients)
	monitor.options = append([]Option(nil), options...)
	return monitor
}

func NewNodeStatusMonitorWithClients(cfg config.Config, roomState *store.RedisRoomStore, clients map[string]Client) *NodeStatusMonitor {
	normalized := make(map[string]Client, len(clients))
	for nodeID, client := range clients {
		if nodeID == "" || client == nil {
			continue
		}
		normalized[nodeID] = client
	}

	return &NodeStatusMonitor{
		cfg:       cfg,
		roomState: roomState,
		clients:   normalized,
	}
}

func (m *NodeStatusMonitor) Start(ctx context.Context) {
	if !m.enabled() {
		return
	}

	go func() {
		m.PollOnce(ctx)

		ticker := time.NewTicker(m.cfg.SFUStatusPollInterval)
		defer ticker.Stop()

		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				m.PollOnce(ctx)
			}
		}
	}()
}

func (m *NodeStatusMonitor) PollOnce(ctx context.Context) {
	if !m.enabled() {
		return
	}

	for _, node := range m.monitorNodes(ctx) {
		client := m.clientForNode(node)
		if node.NodeID == "" || client == nil {
			continue
		}

		rsp, err := client.GetNodeStatus(ctx, &pb.GetNodeStatusReq{})
		if err != nil || rsp == nil || !rsp.GetSuccess() {
			_ = m.roomState.MarkSFUNodeFailure(ctx, node.NodeID, m.cfg.SFUNodeQuarantine)
			_ = m.roomState.ClearSFUNodeStatus(ctx, node.NodeID)
			continue
		}

		_ = m.roomState.ReportSFUNodeStatus(ctx, node.NodeID, store.SFUNodeStatus{
			SFUAddress:     rsp.GetSfuAddress(),
			MediaPort:      rsp.GetMediaPort(),
			RoomCount:      int(rsp.GetRoomCount()),
			PublisherCount: int(rsp.GetPublisherCount()),
			PacketCount:    rsp.GetPacketCount(),
			UpdatedAtUnix:  time.Now().Unix(),
		}, m.statusTTL())
		_ = m.roomState.ClearSFUNodeFailure(ctx, node.NodeID)
	}
}

func (m *NodeStatusMonitor) enabled() bool {
	return m != nil &&
		m.roomState != nil &&
		m.roomState.Enabled() &&
		m.cfg.SFUStatusPollInterval > 0
}

func (m *NodeStatusMonitor) statusTTL() time.Duration {
	ttl := m.cfg.SFUStatusTTL
	if ttl <= m.cfg.SFUStatusPollInterval {
		ttl = m.cfg.SFUStatusPollInterval * 2
	}
	if ttl <= 0 {
		ttl = 20 * time.Second
	}
	return ttl
}

func (m *NodeStatusMonitor) monitorNodes(ctx context.Context) []config.SFUNode {
	if m == nil {
		return nil
	}

	configured := m.cfg.EffectiveSFUNodes()
	if len(m.cfg.SFUNodes) == 0 {
		configured = nil
	}
	if m.roomState == nil {
		if len(configured) > 0 {
			return configured
		}
		return m.cfg.EffectiveSFUNodes()
	}

	resolved, err := m.roomState.EffectiveSFUNodes(ctx, configured)
	if err == nil && len(resolved) > 0 {
		return resolved
	}
	if len(configured) > 0 {
		return configured
	}
	return m.cfg.EffectiveSFUNodes()
}

func (m *NodeStatusMonitor) clientForNode(node config.SFUNode) Client {
	if m == nil || node.NodeID == "" {
		return nil
	}

	m.clientsMu.RLock()
	client := m.clients[node.NodeID]
	m.clientsMu.RUnlock()
	if client != nil {
		return client
	}
	if node.RPCAddress == "" {
		return nil
	}

	created := NewClient(node.RPCAddress, m.options...)
	m.clientsMu.Lock()
	if existing := m.clients[node.NodeID]; existing != nil {
		m.clientsMu.Unlock()
		return existing
	}
	m.clients[node.NodeID] = created
	m.clientsMu.Unlock()
	return created
}
