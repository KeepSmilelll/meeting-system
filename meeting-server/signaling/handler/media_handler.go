package handler

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	pb "meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"
)

type MediaHandler struct {
	cfg          config.Config
	sessions     *server.SessionManager
	meetingStore store.MeetingLifecycleStore
	roomState    *store.RedisRoomStore
	sfuClient    signalingSfu.Client
	sessionStore store.SessionStore
	directBus    store.UserEventPublisher

	recoveryMu    sync.Mutex
	recoveryLocks map[string]*meetingRecoveryLock

	routeStatusMu    sync.Mutex
	routeStatusCache map[string]routeStatusRecord
	routeStatusSweep time.Time
}

type mediaDescription struct {
	AudioSSRC uint32 `json:"audio_ssrc"`
	VideoSSRC uint32 `json:"video_ssrc"`
}

type meetingRecoveryLock struct {
	mu   sync.Mutex
	refs int
}

const (
	routeStatusStageSwitching = "switching"
	routeStatusStageSwitched  = "switched"
	routeStatusStageFailed    = "failed"

	defaultRouteStatusDedupWindow = 3 * time.Second
	routeStatusSweepInterval      = time.Minute
	routeStatusStaleMultiplier    = 2

	recoveryMetricLockAttempts     = "lock_attempts"
	recoveryMetricLockAcquired     = "lock_acquired"
	recoveryMetricLockContended    = "lock_contended"
	recoveryMetricFollowupAttempts = "followup_attempts"
	recoveryMetricFollowupSuccess  = "followup_success"
	recoveryMetricFollowupFailed   = "followup_failed"
	recoveryMetricFailoverAttempts = "failover_attempts"
	recoveryMetricFailoverSuccess  = "failover_success"
	recoveryMetricFailoverFailed   = "failover_failed"
	recoveryMetricRouteStatusSent  = "route_status_emitted"
	recoveryMetricRouteStatusDrop  = "route_status_deduped"

	recoveryNodeMetricAttempts      = "attempts"
	recoveryNodeMetricFailoverOK    = "failover_success"
	recoveryNodeMetricFailoverError = "failover_failed"
)

type mediaRouteStatusEvent struct {
	Stage      string `json:"stage"`
	Message    string `json:"message"`
	FromNodeID string `json:"from_node_id,omitempty"`
	ToNodeID   string `json:"to_node_id,omitempty"`
	Route      string `json:"route,omitempty"`
}

type routeStatusRecord struct {
	fingerprint string
	emittedAt   time.Time
}

func NewMediaHandler(cfg config.Config, sessions *server.SessionManager, meetingStore store.MeetingLifecycleStore, roomState *store.RedisRoomStore, sfuClient signalingSfu.Client, sessionStore store.SessionStore, directBus store.UserEventPublisher) *MediaHandler {
	return &MediaHandler{
		cfg:              cfg,
		sessions:         sessions,
		meetingStore:     meetingStore,
		roomState:        roomState,
		sfuClient:        sfuClient,
		sessionStore:     sessionStore,
		directBus:        directBus,
		routeStatusCache: make(map[string]routeStatusRecord),
	}
}

func (h *MediaHandler) HandleOffer(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaOffer)
}

func (h *MediaHandler) HandleAnswer(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaAnswer)
}

func (h *MediaHandler) HandleIceCandidate(session *server.Session, payload []byte) {
	h.forward(session, payload, protocol.MediaIceCandidate)
}

func (h *MediaHandler) HandleMuteToggle(session *server.Session, payload []byte) {
	if h == nil || session == nil || session.MeetingID() == "" || session.UserID() == "" {
		return
	}

	var req protocol.MediaMuteToggleBody
	if !decodeProto(payload, &req) {
		return
	}

	_ = h.roomState.SetParticipantMediaMuted(context.Background(), session.MeetingID(), session.UserID(), req.MediaType, req.Muted)
	h.broadcastMeetingStateSync(session.MeetingID())
}

func (h *MediaHandler) HandleScreenShare(session *server.Session, payload []byte) {
	if h == nil || session == nil || session.MeetingID() == "" || session.UserID() == "" {
		return
	}

	var req protocol.MediaScreenShareBody
	if !decodeProto(payload, &req) {
		return
	}

	_ = h.roomState.SetParticipantSharing(context.Background(), session.MeetingID(), session.UserID(), req.Sharing)
	h.broadcastMeetingStateSync(session.MeetingID())
}

func (h *MediaHandler) forward(session *server.Session, payload []byte, msgType protocol.SignalType) {
	if h == nil || h.sessions == nil || session == nil {
		return
	}
	if session.MeetingID() == "" || session.UserID() == "" {
		return
	}

	var (
		targetUserID string
		sdp          string
		audioSsrc    uint32
		videoSsrc    uint32
	)

	switch msgType {
	case protocol.MediaOffer:
		var req protocol.MediaOfferBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
		sdp = req.Sdp
		audioSsrc = req.GetAudioSsrc()
		videoSsrc = req.GetVideoSsrc()
	case protocol.MediaAnswer:
		var req protocol.MediaAnswerBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
		sdp = req.Sdp
		audioSsrc = req.GetAudioSsrc()
		videoSsrc = req.GetVideoSsrc()
	case protocol.MediaIceCandidate:
		var req protocol.MediaIceCandidateBody
		if !decodeProto(payload, &req) || req.TargetUserId == "" {
			return
		}
		targetUserID = req.TargetUserId
	default:
		return
	}

	if targetUserID == session.UserID() {
		return
	}

	frame := protocol.EncodeFrame(msgType, payload)
	audioSsrc, videoSsrc = resolveMediaSsrcs(sdp, audioSsrc, videoSsrc)
	h.registerPublisher(session, audioSsrc, videoSsrc)

	if target, ok := h.sessions.GetByUser(targetUserID); ok && target != nil {
		if target.MeetingID() != session.MeetingID() || target.State() != server.StateInMeeting {
			return
		}
		_ = target.SendRaw(frame)
		return
	}

	if h.sessionStore == nil || h.directBus == nil {
		return
	}

	presence, err := h.sessionStore.Get(context.Background(), targetUserID)
	if err != nil || presence == nil {
		return
	}
	if presence.NodeID == "" || presence.NodeID == h.cfg.NodeID {
		return
	}
	if presence.MeetingID != session.MeetingID() || presence.Status != int32(server.StateInMeeting) {
		return
	}

	_ = h.directBus.PublishUserFrame(context.Background(), presence.NodeID, targetUserID, presence.SessionID, frame)
}

func resolveMediaSsrcs(sdp string, audioSsrc, videoSsrc uint32) (uint32, uint32) {
	if (audioSsrc != 0 || videoSsrc != 0) || sdp == "" {
		return audioSsrc, videoSsrc
	}

	var desc mediaDescription
	if err := json.Unmarshal([]byte(sdp), &desc); err != nil {
		return audioSsrc, videoSsrc
	}
	if audioSsrc == 0 {
		audioSsrc = desc.AudioSSRC
	}
	if videoSsrc == 0 {
		videoSsrc = desc.VideoSSRC
	}
	return audioSsrc, videoSsrc
}

func (h *MediaHandler) registerPublisher(session *server.Session, audioSsrc, videoSsrc uint32) {
	if h == nil || h.sfuClient == nil || session == nil {
		return
	}
	if session.MeetingID() == "" || session.UserID() == "" || (audioSsrc == 0 && videoSsrc == 0) {
		return
	}

	ctx := context.Background()
	req := &pb.AddPublisherReq{
		MeetingId: session.MeetingID(),
		UserId:    session.UserID(),
		AudioSsrc: audioSsrc,
		VideoSsrc: videoSsrc,
	}
	rsp, err := h.sfuClient.AddPublisher(ctx, req)
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return
	}
	if err != nil || rsp == nil || !rsp.GetSuccess() {
		_ = h.recoverPublisherRoute(ctx, session, req)
		return
	}
}

func (h *MediaHandler) recoverPublisherRoute(ctx context.Context, session *server.Session, addReq *pb.AddPublisherReq) error {
	if h == nil || session == nil || addReq == nil || h.roomState == nil || !h.roomState.Enabled() {
		return nil
	}

	meetingID := session.MeetingID()
	if meetingID == "" {
		return nil
	}

	release := h.acquireRecoveryLock(meetingID)
	defer release()

	// Another goroutine may have already switched room route. Retry once on the
	// latest route before triggering another failover cycle.
	retryRsp, retryErr := h.sfuClient.AddPublisher(ctx, addReq)
	if errors.Is(retryErr, signalingSfu.ErrDisabled) {
		return nil
	}
	if retryErr == nil && retryRsp != nil && retryRsp.GetSuccess() {
		return nil
	}

	lockOwner := fmt.Sprintf("%s:%s:%d", h.cfg.NodeID, session.UserID(), time.Now().UnixNano())
	releaseDistributed, acquiredDistributed := h.acquireDistributedRecoveryLock(ctx, meetingID, lockOwner)
	defer releaseDistributed()
	if !acquiredDistributed {
		// Another signaling node is recovering this meeting route. Give it a brief
		// chance to update route metadata, then retry once on the latest route.
		h.recordRecoveryMetric(ctx, recoveryMetricFollowupAttempts)
		select {
		case <-ctx.Done():
		case <-time.After(h.recoveryFollowupDelay()):
		}
		followupRsp, followupErr := h.sfuClient.AddPublisher(ctx, addReq)
		if errors.Is(followupErr, signalingSfu.ErrDisabled) {
			return nil
		}
		if followupErr == nil && followupRsp != nil && followupRsp.GetSuccess() {
			h.recordRecoveryMetric(ctx, recoveryMetricFollowupSuccess)
			return nil
		}
		h.recordRecoveryMetric(ctx, recoveryMetricFollowupFailed)
		return nil
	}

	// Route may have changed while waiting for distributed lock ownership.
	lockedRetryRsp, lockedRetryErr := h.sfuClient.AddPublisher(ctx, addReq)
	if errors.Is(lockedRetryErr, signalingSfu.ErrDisabled) {
		return nil
	}
	if lockedRetryErr == nil && lockedRetryRsp != nil && lockedRetryRsp.GetSuccess() {
		return nil
	}

	h.recordRecoveryMetric(ctx, recoveryMetricFailoverAttempts)

	meta, err := h.roomState.RoomMetadata(ctx, meetingID)
	if err != nil || meta == nil {
		return nil
	}
	if meta.SFUNodeID != "" {
		_ = h.roomState.MarkSFUNodeFailure(ctx, meta.SFUNodeID, h.cfg.SFUNodeQuarantine)
	}
	h.recordRecoveryNodeMetric(ctx, meta.SFUNodeID, recoveryNodeMetricAttempts)

	maxPublishers := int32(16)
	if h.meetingStore != nil {
		if meeting, _, snapshotErr := h.meetingStore.SnapshotMeeting(meetingID); snapshotErr == nil && meeting != nil {
			if meeting.MaxParticipants > 0 {
				maxPublishers = int32(meeting.MaxParticipants)
			}
		}
	}

	ranked, err := h.recoveryCandidates(ctx)
	if err != nil {
		h.broadcastRouteStatus(meetingID, mediaRouteStatusEvent{
			Stage:      routeStatusStageFailed,
			Message:    "SFU media route recovery failed",
			FromNodeID: meta.SFUNodeID,
		})
		h.recordRecoveryNodeMetric(ctx, meta.SFUNodeID, recoveryNodeMetricFailoverError)
		return nil
	}
	if len(ranked) == 0 {
		h.broadcastRouteStatus(meetingID, mediaRouteStatusEvent{
			Stage:      routeStatusStageFailed,
			Message:    "SFU media route recovery failed: no healthy backup node",
			FromNodeID: meta.SFUNodeID,
		})
		h.recordRecoveryNodeMetric(ctx, meta.SFUNodeID, recoveryNodeMetricFailoverError)
		return nil
	}

	h.broadcastRouteStatus(meetingID, mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitching,
		Message:    "Switching SFU media route",
		FromNodeID: meta.SFUNodeID,
	})

	var lastErr error
	for _, candidate := range ranked {
		if candidate.NodeID == "" || candidate.MediaAddress == "" {
			continue
		}
		if meta.SFUNodeID != "" && candidate.NodeID == meta.SFUNodeID {
			continue
		}

		client := signalingSfu.ClientForRoute(h.sfuClient, candidate.MediaAddress)
		createRsp, createErr := client.CreateRoom(ctx, &pb.CreateRoomReq{
			MeetingId:     meetingID,
			MaxPublishers: maxPublishers,
		})
		if errors.Is(createErr, signalingSfu.ErrDisabled) {
			return nil
		}
		if createErr != nil || createRsp == nil || !createRsp.GetSuccess() {
			if createErr != nil {
				lastErr = fmt.Errorf("create recovery room on node %s: %w", candidate.NodeID, createErr)
			} else {
				lastErr = fmt.Errorf("create recovery room on node %s: unsuccessful response", candidate.NodeID)
			}
			_ = h.roomState.MarkSFUNodeFailure(ctx, candidate.NodeID, h.cfg.SFUNodeQuarantine)
			h.recordRecoveryNodeMetric(ctx, candidate.NodeID, recoveryNodeMetricFailoverError)
			continue
		}

		addRsp, addErr := client.AddPublisher(ctx, addReq)
		if errors.Is(addErr, signalingSfu.ErrDisabled) {
			return nil
		}
		if addErr != nil || addRsp == nil || !addRsp.GetSuccess() {
			// Roll back the freshly created room on this candidate to avoid
			// leaking orphan SFU rooms in create-success/add-failed scenarios.
			destroyRsp, destroyErr := client.DestroyRoom(ctx, &pb.DestroyRoomReq{MeetingId: meetingID})
			if addErr != nil {
				lastErr = fmt.Errorf("add publisher on recovery node %s: %w", candidate.NodeID, addErr)
			} else {
				lastErr = fmt.Errorf("add publisher on recovery node %s: unsuccessful response", candidate.NodeID)
			}
			if destroyErr != nil {
				lastErr = fmt.Errorf("%w; rollback destroy room on node %s: %v", lastErr, candidate.NodeID, destroyErr)
			} else if destroyRsp == nil || !destroyRsp.GetSuccess() {
				lastErr = fmt.Errorf("%w; rollback destroy room on node %s: unsuccessful response", lastErr, candidate.NodeID)
			}
			_ = h.roomState.MarkSFUNodeFailure(ctx, candidate.NodeID, h.cfg.SFUNodeQuarantine)
			h.recordRecoveryNodeMetric(ctx, candidate.NodeID, recoveryNodeMetricFailoverError)
			continue
		}

		nextRoute := candidate.MediaAddress
		if createRsp.GetSfuAddress() != "" {
			nextRoute = createRsp.GetSfuAddress()
		}
		switched, switchErr := h.roomState.SwitchRoomRoute(ctx, meetingID, meta.SFUNodeID, candidate.NodeID, nextRoute)
		if switchErr != nil {
			lastErr = fmt.Errorf("update recovery room route to node %s: %w", candidate.NodeID, switchErr)
			continue
		}
		if !switched {
			// Room route changed concurrently; prefer latest route metadata and
			// roll back temporary candidate room only when route clearly points
			// elsewhere to avoid destroying an active room selected by peers.
			rollbackErr := error(nil)
			latestMeta, latestMetaErr := h.roomState.RoomMetadata(ctx, meetingID)
			if latestMetaErr == nil && latestMeta != nil && latestMeta.SFUNodeID != "" && latestMeta.SFUNodeID != candidate.NodeID {
				destroyRsp, destroyErr := client.DestroyRoom(ctx, &pb.DestroyRoomReq{MeetingId: meetingID})
				if destroyErr != nil {
					rollbackErr = fmt.Errorf("rollback destroy room on node %s: %w", candidate.NodeID, destroyErr)
				} else if destroyRsp == nil || !destroyRsp.GetSuccess() {
					rollbackErr = fmt.Errorf("rollback destroy room on node %s: unsuccessful response", candidate.NodeID)
				}
			}

			retryRsp, retryErr := h.sfuClient.AddPublisher(ctx, addReq)
			if errors.Is(retryErr, signalingSfu.ErrDisabled) {
				return nil
			}
			if retryErr == nil && retryRsp != nil && retryRsp.GetSuccess() {
				h.recordRecoveryMetric(ctx, recoveryMetricFailoverSuccess)
				return nil
			}
			if retryErr != nil {
				lastErr = fmt.Errorf("room route switched concurrently while failing over to node %s: %w", candidate.NodeID, retryErr)
			} else {
				lastErr = fmt.Errorf("room route switched concurrently while failing over to node %s", candidate.NodeID)
			}
			if rollbackErr != nil {
				lastErr = fmt.Errorf("%w; %v", lastErr, rollbackErr)
			}
			continue
		}
		_ = h.roomState.ClearSFUNodeFailure(ctx, candidate.NodeID)
		h.broadcastRouteStatus(meetingID, mediaRouteStatusEvent{
			Stage:      routeStatusStageSwitched,
			Message:    fmt.Sprintf("SFU media route switched to %s", nextRoute),
			FromNodeID: meta.SFUNodeID,
			ToNodeID:   candidate.NodeID,
			Route:      nextRoute,
		})
		h.recordRecoveryMetric(ctx, recoveryMetricFailoverSuccess)
		h.recordRecoveryNodeMetric(ctx, candidate.NodeID, recoveryNodeMetricFailoverOK)
		return nil
	}
	h.broadcastRouteStatus(meetingID, mediaRouteStatusEvent{
		Stage:      routeStatusStageFailed,
		Message:    "SFU media route recovery failed",
		FromNodeID: meta.SFUNodeID,
	})
	h.recordRecoveryMetric(ctx, recoveryMetricFailoverFailed)
	h.recordRecoveryNodeMetric(ctx, meta.SFUNodeID, recoveryNodeMetricFailoverError)
	return lastErr
}

func (h *MediaHandler) acquireDistributedRecoveryLock(ctx context.Context, meetingID, owner string) (func(), bool) {
	if h == nil || h.roomState == nil || !h.roomState.Enabled() || meetingID == "" || owner == "" {
		return func() {}, true
	}

	h.recordRecoveryMetric(ctx, recoveryMetricLockAttempts)
	acquired, err := h.roomState.AcquireMeetingRecoveryLock(ctx, meetingID, owner, h.recoveryLockTTL())
	if err != nil {
		// Redis lock failures should not block recovery; fallback to local lock only.
		return func() {}, true
	}
	if !acquired {
		h.recordRecoveryMetric(ctx, recoveryMetricLockContended)
		return func() {}, false
	}
	h.recordRecoveryMetric(ctx, recoveryMetricLockAcquired)

	return func() {
		_ = h.roomState.ReleaseMeetingRecoveryLock(context.Background(), meetingID, owner)
	}, true
}

func (h *MediaHandler) recoveryLockTTL() time.Duration {
	if h == nil || h.cfg.SFURecoveryLockTTL <= 0 {
		return 8 * time.Second
	}
	return h.cfg.SFURecoveryLockTTL
}

func (h *MediaHandler) recoveryFollowupDelay() time.Duration {
	if h == nil || h.cfg.SFURecoveryFollowup <= 0 {
		return 250 * time.Millisecond
	}
	return h.cfg.SFURecoveryFollowup
}

func (h *MediaHandler) routeStatusDedupWindow() time.Duration {
	if h == nil || h.cfg.SFURouteStatusDedup <= 0 {
		return defaultRouteStatusDedupWindow
	}
	return h.cfg.SFURouteStatusDedup
}

func (h *MediaHandler) recordRecoveryMetric(ctx context.Context, field string) {
	if h == nil || h.roomState == nil || h.cfg.NodeID == "" || field == "" {
		return
	}
	_ = h.roomState.IncrementRecoveryMetric(ctx, h.cfg.NodeID, field)
}

func (h *MediaHandler) recordRecoveryNodeMetric(ctx context.Context, nodeID, field string) {
	if h == nil || h.roomState == nil || nodeID == "" || field == "" {
		return
	}
	_ = h.roomState.IncrementSFUNodeRecoveryMetric(ctx, nodeID, field)
}

func (h *MediaHandler) acquireRecoveryLock(meetingID string) func() {
	if h == nil || meetingID == "" {
		return func() {}
	}

	h.recoveryMu.Lock()
	if h.recoveryLocks == nil {
		h.recoveryLocks = make(map[string]*meetingRecoveryLock)
	}

	entry, ok := h.recoveryLocks[meetingID]
	if !ok || entry == nil {
		entry = &meetingRecoveryLock{}
		h.recoveryLocks[meetingID] = entry
	}
	entry.refs++
	h.recoveryMu.Unlock()

	entry.mu.Lock()
	return func() {
		entry.mu.Unlock()
		h.recoveryMu.Lock()
		entry.refs--
		if entry.refs <= 0 {
			delete(h.recoveryLocks, meetingID)
		}
		h.recoveryMu.Unlock()
	}
}

func (h *MediaHandler) recoveryCandidates(ctx context.Context) ([]config.SFUNode, error) {
	if h == nil || h.roomState == nil {
		return nil, nil
	}

	nodes := h.cfg.EffectiveSFUNodes()
	configured := nodes
	if len(h.cfg.SFUNodes) == 0 {
		configured = nil
	}

	if resolved, err := h.roomState.EffectiveSFUNodes(ctx, configured); err == nil && len(resolved) > 0 {
		nodes = resolved
	}
	return h.roomState.RankedSFUNodes(ctx, nodes)
}

func (h *MediaHandler) broadcastRouteStatus(meetingID string, event mediaRouteStatusEvent) {
	if h == nil || h.sessions == nil || meetingID == "" {
		return
	}

	ctx := context.Background()
	event = normalizeRouteStatusEvent(event)
	if event.Message == "" {
		return
	}
	if !h.shouldEmitRouteStatus(meetingID, event) {
		h.recordRecoveryMetric(ctx, recoveryMetricRouteStatusDrop)
		return
	}

	reason := encodeRouteStatusReason(event)
	if reason == "" {
		return
	}
	h.recordRecoveryMetric(ctx, recoveryMetricRouteStatusSent)
	h.sessions.BroadcastToRoom(meetingID, protocol.MediaRouteStatusNotify, &protocol.MediaRouteStatusNotifyBody{
		Reason: reason,
	}, "")
}

func normalizeRouteStatusEvent(event mediaRouteStatusEvent) mediaRouteStatusEvent {
	event.Stage = strings.ToLower(strings.TrimSpace(event.Stage))
	switch event.Stage {
	case routeStatusStageSwitching, routeStatusStageSwitched, routeStatusStageFailed:
	default:
		event.Stage = routeStatusStageSwitched
	}

	event.Message = strings.TrimSpace(event.Message)
	if event.Message == "" {
		switch event.Stage {
		case routeStatusStageSwitching:
			event.Message = "Switching SFU media route"
		case routeStatusStageFailed:
			event.Message = "SFU media route recovery failed"
		default:
			event.Message = "SFU media route switched"
		}
	}
	event.FromNodeID = strings.TrimSpace(event.FromNodeID)
	event.ToNodeID = strings.TrimSpace(event.ToNodeID)
	event.Route = strings.TrimSpace(event.Route)
	return event
}

func encodeRouteStatusReason(event mediaRouteStatusEvent) string {
	payload, err := json.Marshal(event)
	if err == nil && len(payload) <= 256 {
		return string(payload)
	}

	msg := event.Message
	if len(msg) > 256 {
		msg = msg[:256]
	}
	return msg
}

func (h *MediaHandler) shouldEmitRouteStatus(meetingID string, event mediaRouteStatusEvent) bool {
	if h == nil || meetingID == "" {
		return false
	}

	now := time.Now()
	fingerprint := fmt.Sprintf("%s|%s|%s|%s", event.Stage, event.FromNodeID, event.ToNodeID, event.Route)

	h.routeStatusMu.Lock()
	defer h.routeStatusMu.Unlock()

	if h.routeStatusCache == nil {
		h.routeStatusCache = make(map[string]routeStatusRecord)
	}
	h.maybeSweepRouteStatusCacheLocked(now)
	if prev, ok := h.routeStatusCache[meetingID]; ok && prev.fingerprint == fingerprint && now.Sub(prev.emittedAt) < h.routeStatusDedupWindow() {
		return false
	}
	h.routeStatusCache[meetingID] = routeStatusRecord{
		fingerprint: fingerprint,
		emittedAt:   now,
	}
	return true
}

func (h *MediaHandler) maybeSweepRouteStatusCacheLocked(now time.Time) {
	if h == nil {
		return
	}
	if !h.routeStatusSweep.IsZero() && now.Sub(h.routeStatusSweep) < routeStatusSweepInterval {
		return
	}
	h.routeStatusSweep = now

	ttl := h.routeStatusDedupWindow() * routeStatusStaleMultiplier
	if ttl <= 0 {
		ttl = defaultRouteStatusDedupWindow * routeStatusStaleMultiplier
	}
	for meetingID, record := range h.routeStatusCache {
		if now.Sub(record.emittedAt) > ttl {
			delete(h.routeStatusCache, meetingID)
		}
	}
}

func (h *MediaHandler) broadcastMeetingStateSync(meetingID string) {
	if h == nil || h.sessions == nil || h.meetingStore == nil || meetingID == "" {
		return
	}

	meeting, participants, err := h.meetingStore.SnapshotMeeting(meetingID)
	if err != nil || meeting == nil {
		return
	}
	participants = h.roomState.HydrateParticipants(context.Background(), meetingID, participants)

	h.sessions.BroadcastToRoom(meetingID, protocol.MeetStateSync, &protocol.MeetStateSyncNotifyBody{
		MeetingId:    meeting.ID,
		Title:        meeting.Title,
		HostId:       meeting.HostUserID,
		Participants: participants,
	}, "")
}
