package store

import (
	"context"
	"fmt"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"sort"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/redis/go-redis/v9"
)

type RedisRoomStore struct {
	enabled    bool
	client     *redis.Client
	ownsClient bool
	localSeq   atomic.Uint64
}

type RoomMemberState struct {
	AudioOn bool
	VideoOn bool
	Sharing bool
}

type RoomMetadata struct {
	HostUserID string
	SFUNodeID  string
	SFUAddress string
	MuteAll    bool
}

type SFUNodeRuntime struct {
	Node         config.SFUNode
	Load         int
	Quarantined  bool
	RotationRank int
}

type SFUNodeStatus struct {
	SFUAddress     string
	MediaPort      uint32
	RoomCount      int
	PublisherCount int
	PacketCount    uint64
	UpdatedAtUnix  int64

	RecoveryAttempts        int64
	RecoveryFailoverSuccess int64
	RecoveryFailoverFailed  int64
	RecoveryUpdatedAtUnix   int64
}

type ParticipantQuality struct {
	PacketLoss    float32
	RttMs         uint32
	JitterMs      uint32
	BitrateKbps   uint32
	UpdatedAtUnix int64
}

type SFURecoveryMetrics struct {
	LockAttempts     int64
	LockAcquired     int64
	LockContended    int64
	FollowupAttempts int64
	FollowupSuccess  int64
	FollowupFailed   int64
	FailoverAttempts int64
	FailoverSuccess  int64
	FailoverFailed   int64
	RouteStatusSent  int64
	RouteStatusDrop  int64
	UpdatedAtUnix    int64
}

var releaseRecoveryLockScript = redis.NewScript(`
if redis.call("GET", KEYS[1]) == ARGV[1] then
	return redis.call("DEL", KEYS[1])
end
return 0
`)

func NewRedisClient(cfg config.Config) *redis.Client {
	if !cfg.EnableRedis {
		return nil
	}

	client := redis.NewClient(&redis.Options{
		Addr:     cfg.RedisAddr,
		Password: cfg.RedisPassword,
		DB:       cfg.RedisDB,
	})

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	if err := client.Ping(ctx).Err(); err != nil {
		_ = client.Close()
		return nil
	}

	return client
}

func NewRedisRoomStore(cfg config.Config) *RedisRoomStore {
	client := NewRedisClient(cfg)
	if client == nil {
		return &RedisRoomStore{enabled: false}
	}
	return &RedisRoomStore{enabled: true, client: client, ownsClient: true}
}

func NewRedisRoomStoreWithClient(client *redis.Client) *RedisRoomStore {
	if client == nil {
		return &RedisRoomStore{enabled: false}
	}
	return &RedisRoomStore{enabled: true, client: client, ownsClient: false}
}

func (r *RedisRoomStore) Enabled() bool {
	return r != nil && r.enabled
}

func (r *RedisRoomStore) Close() error {
	if !r.Enabled() || !r.ownsClient {
		return nil
	}
	return r.client.Close()
}

func (r *RedisRoomStore) UpsertRoom(ctx context.Context, meetingID, hostUserID, sfuNodeID, sfuAddr string) error {
	if !r.Enabled() {
		return nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	routeKey := fmt.Sprintf("sfu_route:%s", meetingID)
	prevNodeID := ""
	if meetingID != "" {
		value, err := r.client.HGet(ctx, key, "sfu_node_id").Result()
		if err != nil && err != redis.Nil {
			return err
		}
		if err == nil {
			prevNodeID = value
		}
	}

	pipe := r.client.TxPipeline()
	pipe.HSet(ctx, key, map[string]any{
		"host_id":     hostUserID,
		"mute_all":    0,
		"sfu_node_id": sfuNodeID,
		"status":      "1",
		"created_at":  strconv.FormatInt(time.Now().Unix(), 10),
	})
	if sfuAddr != "" {
		pipe.Set(ctx, routeKey, sfuAddr, 0)
	}
	if prevNodeID != sfuNodeID {
		if prevNodeID != "" {
			pipe.DecrBy(ctx, sfuNodeLoadKey(prevNodeID), 1)
		}
		if sfuNodeID != "" {
			pipe.IncrBy(ctx, sfuNodeLoadKey(sfuNodeID), 1)
		}
	}

	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) SwitchRoomRoute(ctx context.Context, meetingID, expectedNodeID, nextNodeID, nextRoute string) (bool, error) {
	if !r.Enabled() || meetingID == "" {
		return true, nil
	}

	roomKey := fmt.Sprintf("room:%s", meetingID)
	routeKey := fmt.Sprintf("sfu_route:%s", meetingID)

	var switched bool
	for attempt := 0; attempt < 3; attempt++ {
		if err := r.client.Watch(ctx, func(tx *redis.Tx) error {
			currentNodeID, err := tx.HGet(ctx, roomKey, "sfu_node_id").Result()
			if err == redis.Nil {
				currentNodeID = ""
			} else if err != nil {
				return err
			}

			if expectedNodeID != "" && currentNodeID != expectedNodeID {
				switched = false
				return nil
			}

			pipe := tx.TxPipeline()
			pipe.HSet(ctx, roomKey, "sfu_node_id", nextNodeID)
			if nextRoute != "" {
				pipe.Set(ctx, routeKey, nextRoute, 0)
			} else {
				pipe.Del(ctx, routeKey)
			}
			if currentNodeID != nextNodeID {
				if currentNodeID != "" {
					pipe.DecrBy(ctx, sfuNodeLoadKey(currentNodeID), 1)
				}
				if nextNodeID != "" {
					pipe.IncrBy(ctx, sfuNodeLoadKey(nextNodeID), 1)
				}
			}
			if _, err := pipe.Exec(ctx); err != nil {
				return err
			}
			switched = true
			return nil
		}, roomKey); err != nil {
			if err == redis.TxFailedErr {
				continue
			}
			return false, err
		}
		return switched, nil
	}

	return false, nil
}

func (r *RedisRoomStore) TransferHost(ctx context.Context, meetingID, newHostID string) error {
	if !r.Enabled() {
		return nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	return r.client.HSet(ctx, key, "host_id", newHostID).Err()
}

func (r *RedisRoomStore) SetMuteAll(ctx context.Context, meetingID string, mute bool) error {
	if !r.Enabled() {
		return nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	return r.client.HSet(ctx, key, "mute_all", boolToInt(mute)).Err()
}

func (r *RedisRoomStore) MuteAll(ctx context.Context, meetingID string) (bool, error) {
	if !r.Enabled() {
		return false, nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	value, err := r.client.HGet(ctx, key, "mute_all").Result()
	if err == redis.Nil {
		return false, nil
	}
	if err != nil {
		return false, err
	}

	parsed, err := strconv.Atoi(value)
	if err != nil {
		return false, fmt.Errorf("parse mute_all %q: %w", value, err)
	}
	return parsed != 0, nil
}

func (r *RedisRoomStore) RoomMetadata(ctx context.Context, meetingID string) (*RoomMetadata, error) {
	if !r.Enabled() || meetingID == "" {
		return nil, nil
	}

	roomKey := fmt.Sprintf("room:%s", meetingID)
	routeKey := fmt.Sprintf("sfu_route:%s", meetingID)

	pipe := r.client.Pipeline()
	roomCmd := pipe.HGetAll(ctx, roomKey)
	routeCmd := pipe.Get(ctx, routeKey)
	if _, err := pipe.Exec(ctx); err != nil && err != redis.Nil {
		return nil, err
	}

	roomValues, err := roomCmd.Result()
	if err != nil {
		return nil, err
	}
	if len(roomValues) == 0 {
		return nil, nil
	}

	route := ""
	if value, err := routeCmd.Result(); err == nil {
		route = value
	} else if err != redis.Nil {
		return nil, err
	}

	meta := &RoomMetadata{
		HostUserID: roomValues["host_id"],
		SFUNodeID:  roomValues["sfu_node_id"],
		SFUAddress: route,
		MuteAll:    roomValues["mute_all"] != "0",
	}
	return meta, nil
}

func (r *RedisRoomStore) NextSFUNode(ctx context.Context, nodes []config.SFUNode) (*config.SFUNode, error) {
	ranked, err := r.RankedSFUNodes(ctx, nodes)
	if err != nil || len(ranked) == 0 {
		return nil, err
	}
	return &ranked[0], nil
}

func (r *RedisRoomStore) RankedSFUNodes(ctx context.Context, nodes []config.SFUNode) ([]config.SFUNode, error) {
	if len(nodes) == 0 {
		return nil, nil
	}

	index, err := r.nextSFUAllocationIndex(ctx)
	if err != nil {
		return nil, err
	}
	if !r.Enabled() {
		return rotateSFUNodes(nodes, int(index)%len(nodes)), nil
	}

	pipe := r.client.Pipeline()
	loadCmds := make([]*redis.StringCmd, len(nodes))
	quarantineCmds := make([]*redis.IntCmd, len(nodes))
	for i, node := range nodes {
		loadCmds[i] = pipe.Get(ctx, sfuNodeLoadKey(node.NodeID))
		quarantineCmds[i] = pipe.Exists(ctx, sfuNodeQuarantineKey(node.NodeID))
	}
	if _, err := pipe.Exec(ctx); err != nil && err != redis.Nil {
		return nil, err
	}

	rotationStart := int(index % uint64(len(nodes)))
	runtimes := make([]SFUNodeRuntime, 0, len(nodes))
	for i, node := range nodes {
		load := 0
		if raw, err := loadCmds[i].Result(); err == nil {
			if parsed, parseErr := strconv.Atoi(raw); parseErr == nil && parsed > 0 {
				load = parsed
			}
		}

		quarantined := false
		if exists, err := quarantineCmds[i].Result(); err == nil {
			quarantined = exists > 0
		}

		runtimes = append(runtimes, SFUNodeRuntime{
			Node:         node,
			Load:         load,
			Quarantined:  quarantined,
			RotationRank: (i - rotationStart + len(nodes)) % len(nodes),
		})
	}

	sort.SliceStable(runtimes, func(i, j int) bool {
		leftTier := sfuNodePriorityTier(runtimes[i])
		rightTier := sfuNodePriorityTier(runtimes[j])
		if leftTier != rightTier {
			return leftTier < rightTier
		}
		if runtimes[i].Load != runtimes[j].Load {
			return runtimes[i].Load < runtimes[j].Load
		}
		return runtimes[i].RotationRank < runtimes[j].RotationRank
	})

	ordered := make([]config.SFUNode, 0, len(runtimes))
	for _, runtime := range runtimes {
		ordered = append(ordered, runtime.Node)
	}
	return ordered, nil
}

func (r *RedisRoomStore) MarkSFUNodeFailure(ctx context.Context, nodeID string, ttl time.Duration) error {
	if !r.Enabled() || nodeID == "" || ttl <= 0 {
		return nil
	}
	return r.client.Set(ctx, sfuNodeQuarantineKey(nodeID), strconv.FormatInt(time.Now().Unix(), 10), ttl).Err()
}

func (r *RedisRoomStore) AcquireMeetingRecoveryLock(ctx context.Context, meetingID, owner string, ttl time.Duration) (bool, error) {
	if !r.Enabled() || meetingID == "" || owner == "" {
		return true, nil
	}
	if ttl <= 0 {
		ttl = 8 * time.Second
	}

	acquired, err := r.client.SetNX(ctx, sfuRecoveryLockKey(meetingID), owner, ttl).Result()
	if err != nil {
		return false, err
	}
	return acquired, nil
}

func (r *RedisRoomStore) ReleaseMeetingRecoveryLock(ctx context.Context, meetingID, owner string) error {
	if !r.Enabled() || meetingID == "" || owner == "" {
		return nil
	}

	if _, err := releaseRecoveryLockScript.Run(ctx, r.client, []string{sfuRecoveryLockKey(meetingID)}, owner).Result(); err != nil && err != redis.Nil {
		return err
	}
	return nil
}

func (r *RedisRoomStore) IncrementRecoveryMetric(ctx context.Context, nodeID, field string) error {
	if !r.Enabled() || nodeID == "" || field == "" {
		return nil
	}

	key := sfuRecoveryMetricsKey(nodeID)
	now := strconv.FormatInt(time.Now().Unix(), 10)
	pipe := r.client.TxPipeline()
	pipe.HIncrBy(ctx, key, field, 1)
	pipe.HSet(ctx, key, "updated_at", now)
	pipe.Expire(ctx, key, 7*24*time.Hour)
	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) LoadRecoveryMetrics(ctx context.Context, nodeID string) (*SFURecoveryMetrics, error) {
	if !r.Enabled() || nodeID == "" {
		return nil, nil
	}

	values, err := r.client.HGetAll(ctx, sfuRecoveryMetricsKey(nodeID)).Result()
	if err != nil {
		return nil, err
	}
	if len(values) == 0 {
		return nil, nil
	}

	metrics := &SFURecoveryMetrics{
		LockAttempts:     parseInt64(values["lock_attempts"]),
		LockAcquired:     parseInt64(values["lock_acquired"]),
		LockContended:    parseInt64(values["lock_contended"]),
		FollowupAttempts: parseInt64(values["followup_attempts"]),
		FollowupSuccess:  parseInt64(values["followup_success"]),
		FollowupFailed:   parseInt64(values["followup_failed"]),
		FailoverAttempts: parseInt64(values["failover_attempts"]),
		FailoverSuccess:  parseInt64(values["failover_success"]),
		FailoverFailed:   parseInt64(values["failover_failed"]),
		RouteStatusSent:  parseInt64(values["route_status_emitted"]),
		RouteStatusDrop:  parseInt64(values["route_status_deduped"]),
		UpdatedAtUnix:    parseInt64(values["updated_at"]),
	}
	return metrics, nil
}

func (r *RedisRoomStore) IncrementSFUNodeRecoveryMetric(ctx context.Context, nodeID, field string) error {
	if !r.Enabled() || nodeID == "" || field == "" {
		return nil
	}

	key := sfuNodeRecoveryKey(nodeID)
	now := strconv.FormatInt(time.Now().Unix(), 10)
	pipe := r.client.TxPipeline()
	pipe.HIncrBy(ctx, key, field, 1)
	pipe.HSet(ctx, key, "updated_at", now)
	pipe.Expire(ctx, key, 7*24*time.Hour)
	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) RecoveryMetricNodeIDs(ctx context.Context) ([]string, error) {
	if !r.Enabled() {
		return nil, nil
	}

	const pattern = "sfu_node_recovery:*"
	const prefix = "sfu_node_recovery:"
	seen := make(map[string]struct{})
	var (
		cursor uint64
		err    error
	)
	for {
		var keys []string
		keys, cursor, err = r.client.Scan(ctx, cursor, pattern, 100).Result()
		if err != nil {
			return nil, err
		}
		for _, key := range keys {
			nodeID := strings.TrimPrefix(key, prefix)
			if nodeID == "" || nodeID == key {
				continue
			}
			seen[nodeID] = struct{}{}
		}
		if cursor == 0 {
			break
		}
	}

	nodeIDs := make([]string, 0, len(seen))
	for nodeID := range seen {
		nodeIDs = append(nodeIDs, nodeID)
	}
	sort.Strings(nodeIDs)
	return nodeIDs, nil
}

func (r *RedisRoomStore) ClearSFUNodeFailure(ctx context.Context, nodeID string) error {
	if !r.Enabled() || nodeID == "" {
		return nil
	}
	return r.client.Del(ctx, sfuNodeQuarantineKey(nodeID)).Err()
}

func (r *RedisRoomStore) ReportSFUNode(ctx context.Context, node config.SFUNode, status SFUNodeStatus, ttl time.Duration) error {
	if !r.Enabled() || node.NodeID == "" {
		return nil
	}

	if ttl <= 0 {
		ttl = 20 * time.Second
	}
	if status.UpdatedAtUnix == 0 {
		status.UpdatedAtUnix = time.Now().Unix()
	}

	metaKey := sfuNodeMetaKey(node.NodeID)
	pipe := r.client.TxPipeline()
	pipe.SAdd(ctx, sfuNodeSetKey(), node.NodeID)
	pipe.HSet(ctx, metaKey, map[string]any{
		"node_id":       node.NodeID,
		"media_address": node.MediaAddress,
		"rpc_address":   node.RPCAddress,
		"max_meetings":  node.MaxMeetings,
		"updated_at":    status.UpdatedAtUnix,
	})
	pipe.Expire(ctx, metaKey, ttl)
	if node.MediaAddress != "" && node.RPCAddress != "" {
		pipe.Set(ctx, sfuNodeRouteRPCKey(node.MediaAddress), node.RPCAddress, ttl)
	}

	pipe.HSet(ctx, sfuNodeStatusKey(node.NodeID), map[string]any{
		"sfu_address":     status.SFUAddress,
		"media_port":      status.MediaPort,
		"room_count":      status.RoomCount,
		"publisher_count": status.PublisherCount,
		"packet_count":    status.PacketCount,
		"updated_at":      status.UpdatedAtUnix,
	})
	pipe.Expire(ctx, sfuNodeStatusKey(node.NodeID), ttl)
	pipe.Set(ctx, sfuNodeLoadKey(node.NodeID), strconv.Itoa(status.RoomCount), 0)

	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) ReportSFUNodeStatus(ctx context.Context, nodeID string, status SFUNodeStatus, ttl time.Duration) error {
	return r.ReportSFUNode(ctx, config.SFUNode{NodeID: nodeID}, status, ttl)
}

func (r *RedisRoomStore) SFUNodeStatus(ctx context.Context, nodeID string) (*SFUNodeStatus, error) {
	if !r.Enabled() || nodeID == "" {
		return nil, nil
	}

	pipe := r.client.Pipeline()
	statusCmd := pipe.HGetAll(ctx, sfuNodeStatusKey(nodeID))
	recoveryCmd := pipe.HGetAll(ctx, sfuNodeRecoveryKey(nodeID))
	if _, err := pipe.Exec(ctx); err != nil && err != redis.Nil {
		return nil, err
	}

	values, err := statusCmd.Result()
	if err != nil {
		return nil, err
	}
	recoveryValues, err := recoveryCmd.Result()
	if err != nil {
		return nil, err
	}
	if len(values) == 0 && len(recoveryValues) == 0 {
		return nil, nil
	}

	status := &SFUNodeStatus{
		SFUAddress: values["sfu_address"],
	}
	if parsed, err := strconv.ParseUint(values["media_port"], 10, 32); err == nil {
		status.MediaPort = uint32(parsed)
	}
	if parsed, err := strconv.Atoi(values["room_count"]); err == nil {
		status.RoomCount = parsed
	}
	if parsed, err := strconv.Atoi(values["publisher_count"]); err == nil {
		status.PublisherCount = parsed
	}
	if parsed, err := strconv.ParseUint(values["packet_count"], 10, 64); err == nil {
		status.PacketCount = parsed
	}
	if parsed, err := strconv.ParseInt(values["updated_at"], 10, 64); err == nil {
		status.UpdatedAtUnix = parsed
	}
	status.RecoveryAttempts = parseInt64(recoveryValues["attempts"])
	status.RecoveryFailoverSuccess = parseInt64(recoveryValues["failover_success"])
	status.RecoveryFailoverFailed = parseInt64(recoveryValues["failover_failed"])
	status.RecoveryUpdatedAtUnix = parseInt64(recoveryValues["updated_at"])
	return status, nil
}

func (r *RedisRoomStore) ClearSFUNodeStatus(ctx context.Context, nodeID string) error {
	if !r.Enabled() || nodeID == "" {
		return nil
	}
	return r.client.Del(ctx, sfuNodeStatusKey(nodeID)).Err()
}

func (r *RedisRoomStore) RegisteredSFUNodes(ctx context.Context) ([]config.SFUNode, error) {
	if !r.Enabled() {
		return nil, nil
	}

	nodeIDs, err := r.client.SMembers(ctx, sfuNodeSetKey()).Result()
	if err != nil {
		return nil, err
	}
	if len(nodeIDs) == 0 {
		return nil, nil
	}

	pipe := r.client.Pipeline()
	cmds := make([]*redis.MapStringStringCmd, 0, len(nodeIDs))
	activeIDs := make([]string, 0, len(nodeIDs))
	for _, nodeID := range nodeIDs {
		if nodeID == "" {
			continue
		}
		activeIDs = append(activeIDs, nodeID)
		cmds = append(cmds, pipe.HGetAll(ctx, sfuNodeMetaKey(nodeID)))
	}
	if _, err := pipe.Exec(ctx); err != nil && err != redis.Nil {
		return nil, err
	}

	nodes := make([]config.SFUNode, 0, len(activeIDs))
	for i, nodeID := range activeIDs {
		values, err := cmds[i].Result()
		if err != nil || len(values) == 0 {
			_ = r.client.SRem(ctx, sfuNodeSetKey(), nodeID).Err()
			continue
		}

		maxMeetings, _ := strconv.Atoi(values["max_meetings"])
		node := config.SFUNode{
			NodeID:       nodeID,
			MediaAddress: values["media_address"],
			RPCAddress:   values["rpc_address"],
			MaxMeetings:  maxMeetings,
		}
		if node.MediaAddress == "" || node.RPCAddress == "" {
			continue
		}
		nodes = append(nodes, node)
	}
	sort.Slice(nodes, func(i, j int) bool {
		return nodes[i].NodeID < nodes[j].NodeID
	})
	return nodes, nil
}

func (r *RedisRoomStore) EffectiveSFUNodes(ctx context.Context, configured []config.SFUNode) ([]config.SFUNode, error) {
	if !r.Enabled() {
		return configured, nil
	}

	registered, err := r.RegisteredSFUNodes(ctx)
	if err != nil {
		return nil, err
	}
	if len(registered) == 0 {
		return configured, nil
	}

	merged := make([]config.SFUNode, 0, len(configured)+len(registered))
	seen := make(map[string]int)
	for _, node := range configured {
		if node.NodeID == "" {
			continue
		}
		seen[node.NodeID] = len(merged)
		merged = append(merged, node)
	}
	for _, node := range registered {
		if node.NodeID == "" {
			continue
		}
		if idx, ok := seen[node.NodeID]; ok {
			merged[idx] = node
			continue
		}
		seen[node.NodeID] = len(merged)
		merged = append(merged, node)
	}
	return merged, nil
}

func (r *RedisRoomStore) RPCAddressForRoute(ctx context.Context, route string) (string, error) {
	if !r.Enabled() || route == "" {
		return "", nil
	}

	value, err := r.client.Get(ctx, sfuNodeRouteRPCKey(route)).Result()
	if err == redis.Nil {
		return "", nil
	}
	if err != nil {
		return "", err
	}
	return value, nil
}

func (r *RedisRoomStore) AddMember(ctx context.Context, meetingID string, p *protocol.Participant) error {
	if !r.Enabled() || p == nil {
		return nil
	}

	setKey := fmt.Sprintf("room_members:%s", meetingID)
	detailKey := fmt.Sprintf("room_member:%s:%s", meetingID, p.UserId)

	pipe := r.client.TxPipeline()
	pipe.SAdd(ctx, setKey, p.UserId)
	pipe.HSet(ctx, detailKey, map[string]any{
		"display_name": p.DisplayName,
		"role":         p.Role,
		"is_audio_on":  boolToInt(p.IsAudioOn),
		"is_video_on":  boolToInt(p.IsVideoOn),
		"is_sharing":   boolToInt(p.IsSharing),
	})

	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) RemoveMember(ctx context.Context, meetingID, userID string) error {
	if !r.Enabled() {
		return nil
	}

	setKey := fmt.Sprintf("room_members:%s", meetingID)
	detailKey := fmt.Sprintf("room_member:%s:%s", meetingID, userID)

	pipe := r.client.TxPipeline()
	pipe.SRem(ctx, setKey, userID)
	pipe.Del(ctx, detailKey)

	_, err := pipe.Exec(ctx)
	return err
}

func (r *RedisRoomStore) SetParticipantMediaMuted(ctx context.Context, meetingID, userID string, mediaType int32, muted bool) error {
	if !r.Enabled() {
		return nil
	}

	field := ""
	switch mediaType {
	case 0:
		field = "is_audio_on"
	case 1:
		field = "is_video_on"
	default:
		return nil
	}

	detailKey := fmt.Sprintf("room_member:%s:%s", meetingID, userID)
	return r.client.HSet(ctx, detailKey, field, boolToInt(!muted)).Err()
}

func (r *RedisRoomStore) SetParticipantSharing(ctx context.Context, meetingID, userID string, sharing bool) error {
	if !r.Enabled() {
		return nil
	}

	detailKey := fmt.Sprintf("room_member:%s:%s", meetingID, userID)
	return r.client.HSet(ctx, detailKey, "is_sharing", boolToInt(sharing)).Err()
}

func (r *RedisRoomStore) ReportParticipantQuality(ctx context.Context, meetingID, userID string, quality ParticipantQuality) error {
	if !r.Enabled() || meetingID == "" || userID == "" {
		return nil
	}

	if quality.UpdatedAtUnix == 0 {
		quality.UpdatedAtUnix = time.Now().Unix()
	}

	detailKey := fmt.Sprintf("room_member:%s:%s", meetingID, userID)
	return r.client.HSet(ctx, detailKey, map[string]any{
		"quality_packet_loss":  strconv.FormatFloat(float64(quality.PacketLoss), 'f', -1, 32),
		"quality_rtt_ms":       quality.RttMs,
		"quality_jitter_ms":    quality.JitterMs,
		"quality_bitrate_kbps": quality.BitrateKbps,
		"quality_updated_at":   quality.UpdatedAtUnix,
	}).Err()
}

func (r *RedisRoomStore) LoadParticipantQuality(ctx context.Context, meetingID, userID string) (*ParticipantQuality, error) {
	if !r.Enabled() || meetingID == "" || userID == "" {
		return nil, nil
	}

	values, err := r.client.HGetAll(ctx, fmt.Sprintf("room_member:%s:%s", meetingID, userID)).Result()
	if err != nil {
		return nil, err
	}
	if len(values) == 0 {
		return nil, nil
	}

	if values["quality_packet_loss"] == "" &&
		values["quality_rtt_ms"] == "" &&
		values["quality_jitter_ms"] == "" &&
		values["quality_bitrate_kbps"] == "" &&
		values["quality_updated_at"] == "" {
		return nil, nil
	}

	quality := &ParticipantQuality{}
	if parsed, err := strconv.ParseFloat(values["quality_packet_loss"], 32); err == nil {
		quality.PacketLoss = float32(parsed)
	}
	if parsed, err := strconv.ParseUint(values["quality_rtt_ms"], 10, 32); err == nil {
		quality.RttMs = uint32(parsed)
	}
	if parsed, err := strconv.ParseUint(values["quality_jitter_ms"], 10, 32); err == nil {
		quality.JitterMs = uint32(parsed)
	}
	if parsed, err := strconv.ParseUint(values["quality_bitrate_kbps"], 10, 32); err == nil {
		quality.BitrateKbps = uint32(parsed)
	}
	if parsed, err := strconv.ParseInt(values["quality_updated_at"], 10, 64); err == nil {
		quality.UpdatedAtUnix = parsed
	}
	return quality, nil
}

func (r *RedisRoomStore) HydrateParticipants(ctx context.Context, meetingID string, participants []*protocol.Participant) []*protocol.Participant {
	if !r.Enabled() || meetingID == "" || len(participants) == 0 {
		return cloneParticipantSlice(participants)
	}

	pipe := r.client.Pipeline()
	cmds := make([]*redis.MapStringStringCmd, len(participants))
	for i, participant := range participants {
		if participant == nil || participant.UserId == "" {
			continue
		}
		cmds[i] = pipe.HGetAll(ctx, fmt.Sprintf("room_member:%s:%s", meetingID, participant.UserId))
	}
	if _, err := pipe.Exec(ctx); err != nil && err != redis.Nil {
		return cloneParticipantSlice(participants)
	}

	result := cloneParticipantSlice(participants)
	for i, participant := range result {
		cmd := cmds[i]
		if participant == nil || cmd == nil {
			continue
		}
		values, err := cmd.Result()
		if err != nil || len(values) == 0 {
			continue
		}
		if value, ok := values["is_audio_on"]; ok {
			participant.IsAudioOn = value != "0"
		}
		if value, ok := values["is_video_on"]; ok {
			participant.IsVideoOn = value != "0"
		}
		if value, ok := values["is_sharing"]; ok {
			participant.IsSharing = value != "0"
		}
	}

	return result
}

func (r *RedisRoomStore) DeleteRoom(ctx context.Context, meetingID string) error {
	if !r.Enabled() {
		return nil
	}

	setKey := fmt.Sprintf("room_members:%s", meetingID)
	roomKey := fmt.Sprintf("room:%s", meetingID)
	routeKey := fmt.Sprintf("sfu_route:%s", meetingID)
	nodeID := ""
	if meetingID != "" {
		value, err := r.client.HGet(ctx, roomKey, "sfu_node_id").Result()
		if err != nil && err != redis.Nil {
			return err
		}
		if err == nil {
			nodeID = value
		}
	}

	members, err := r.client.SMembers(ctx, setKey).Result()
	if err != nil && err != redis.Nil {
		return err
	}

	pipe := r.client.TxPipeline()
	pipe.Del(ctx, setKey)
	pipe.Del(ctx, roomKey)
	pipe.Del(ctx, routeKey)
	if nodeID != "" {
		pipe.DecrBy(ctx, sfuNodeLoadKey(nodeID), 1)
	}
	for _, uid := range members {
		pipe.Del(ctx, fmt.Sprintf("room_member:%s:%s", meetingID, uid))
	}

	_, err = pipe.Exec(ctx)
	return err
}

func boolToInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

func cloneParticipantSlice(participants []*protocol.Participant) []*protocol.Participant {
	if len(participants) == 0 {
		return nil
	}

	out := make([]*protocol.Participant, 0, len(participants))
	for _, participant := range participants {
		if participant == nil {
			out = append(out, nil)
			continue
		}
		copy := *participant
		out = append(out, &copy)
	}
	return out
}

func parseInt64(raw string) int64 {
	if raw == "" {
		return 0
	}
	parsed, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		return 0
	}
	return parsed
}

func (r *RedisRoomStore) nextSFUAllocationIndex(ctx context.Context) (uint64, error) {
	if r != nil && r.Enabled() {
		seq, err := r.client.Incr(ctx, "sfu_alloc_seq").Result()
		if err != nil {
			return 0, err
		}
		return uint64(seq - 1), nil
	}
	if r == nil {
		return 0, nil
	}
	return r.localSeq.Add(1) - 1, nil
}

func rotateSFUNodes(nodes []config.SFUNode, start int) []config.SFUNode {
	if len(nodes) == 0 {
		return nil
	}
	ordered := make([]config.SFUNode, 0, len(nodes))
	for i := 0; i < len(nodes); i++ {
		ordered = append(ordered, nodes[(start+i)%len(nodes)])
	}
	return ordered
}

func sfuNodePriorityTier(runtime SFUNodeRuntime) int {
	underCapacity := runtime.Node.MaxMeetings <= 0 || runtime.Load < runtime.Node.MaxMeetings
	switch {
	case !runtime.Quarantined && underCapacity:
		return 0
	case !runtime.Quarantined:
		return 1
	case underCapacity:
		return 2
	default:
		return 3
	}
}

func sfuNodeLoadKey(nodeID string) string {
	return fmt.Sprintf("sfu_node_load:%s", nodeID)
}

func sfuNodeQuarantineKey(nodeID string) string {
	return fmt.Sprintf("sfu_node_quarantine:%s", nodeID)
}

func sfuNodeStatusKey(nodeID string) string {
	return fmt.Sprintf("sfu_node_status:%s", nodeID)
}

func sfuNodeMetaKey(nodeID string) string {
	return fmt.Sprintf("sfu_node_meta:%s", nodeID)
}

func sfuNodeSetKey() string {
	return "sfu_nodes"
}

func sfuNodeRouteRPCKey(route string) string {
	return fmt.Sprintf("sfu_route_rpc:%s", route)
}

func sfuRecoveryLockKey(meetingID string) string {
	return fmt.Sprintf("sfu_recovery_lock:%s", meetingID)
}

func sfuRecoveryMetricsKey(nodeID string) string {
	return fmt.Sprintf("sfu_recovery_metrics:%s", nodeID)
}

func sfuNodeRecoveryKey(nodeID string) string {
	return fmt.Sprintf("sfu_node_recovery:%s", nodeID)
}
