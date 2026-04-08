package store

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"sync"

	"meeting-server/signaling/config"

	"github.com/redis/go-redis/v9"
)

type UserEventHandler func(event UserNodeEvent)

type UserEventPublisher interface {
	PublishUserFrame(ctx context.Context, nodeID, userID string, targetSessionID uint64, frame []byte) error
	PublishUserControl(ctx context.Context, event UserNodeEvent) error
}

type UserNodeEvent struct {
	TargetNodeID    string
	TargetUserID    string
	TargetSessionID uint64
	Frame           []byte
	ResetMeeting    bool
	State           *int32
	Close           bool
}

type RedisNodeEventBus struct {
	enabled bool
	client  *redis.Client
	nodeID  string

	mu     sync.Mutex
	pubsub *redis.PubSub
	cancel context.CancelFunc
	done   chan struct{}
}

type nodeFrameEnvelope struct {
	TargetUserID    string `json:"target_user_id"`
	TargetSessionID string `json:"target_session_id"`
	Frame           []byte `json:"frame,omitempty"`
	ResetMeeting    bool   `json:"reset_meeting,omitempty"`
	State           *int32 `json:"state,omitempty"`
	Close           bool   `json:"close,omitempty"`
}

func NewRedisNodeEventBusWithClient(cfg config.Config, client *redis.Client) *RedisNodeEventBus {
	if client == nil || !cfg.EnableRedis || cfg.NodeID == "" {
		return &RedisNodeEventBus{}
	}

	return &RedisNodeEventBus{
		enabled: true,
		client:  client,
		nodeID:  cfg.NodeID,
	}
}

func (b *RedisNodeEventBus) Enabled() bool {
	return b != nil && b.enabled
}

func (b *RedisNodeEventBus) Start(ctx context.Context, handler UserEventHandler) error {
	if !b.Enabled() || handler == nil {
		return nil
	}

	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pubsub != nil {
		return nil
	}

	runCtx, cancel := context.WithCancel(ctx)
	pubsub := b.client.Subscribe(runCtx, nodeBroadcastChannel(b.nodeID))
	if _, err := pubsub.Receive(runCtx); err != nil {
		cancel()
		_ = pubsub.Close()
		return fmt.Errorf("node event bus subscribe: %w", err)
	}

	done := make(chan struct{})
	b.pubsub = pubsub
	b.cancel = cancel
	b.done = done

	go b.consume(runCtx, pubsub, handler, done)
	return nil
}

func (b *RedisNodeEventBus) PublishUserFrame(ctx context.Context, nodeID, userID string, targetSessionID uint64, frame []byte) error {
	if !b.Enabled() || nodeID == "" {
		return nil
	}

	payload, err := marshalNodeFrameEnvelope(nodeFrameEnvelope{
		TargetUserID:    userID,
		TargetSessionID: strconv.FormatUint(targetSessionID, 10),
		Frame:           frame,
	})
	if err != nil {
		return fmt.Errorf("node event bus marshal: %w", err)
	}

	if err := b.client.Publish(ctx, nodeBroadcastChannel(nodeID), payload).Err(); err != nil {
		return fmt.Errorf("node event bus publish: %w", err)
	}
	return nil
}

func (b *RedisNodeEventBus) PublishUserControl(ctx context.Context, event UserNodeEvent) error {
	if !b.Enabled() || event.TargetNodeID == "" || event.TargetUserID == "" || event.TargetSessionID == 0 {
		return nil
	}

	payload, err := marshalNodeFrameEnvelope(nodeFrameEnvelope{
		TargetUserID:    event.TargetUserID,
		TargetSessionID: strconv.FormatUint(event.TargetSessionID, 10),
		Frame:           event.Frame,
		ResetMeeting:    event.ResetMeeting,
		State:           event.State,
		Close:           event.Close,
	})
	if err != nil {
		return fmt.Errorf("node event bus marshal control: %w", err)
	}

	if err := b.client.Publish(ctx, nodeBroadcastChannel(event.TargetNodeID), payload).Err(); err != nil {
		return fmt.Errorf("node event bus publish control: %w", err)
	}
	return nil
}

func (b *RedisNodeEventBus) Close() error {
	if b == nil {
		return nil
	}

	b.mu.Lock()
	pubsub := b.pubsub
	cancel := b.cancel
	done := b.done
	b.pubsub = nil
	b.cancel = nil
	b.done = nil
	b.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	if pubsub != nil {
		_ = pubsub.Close()
	}
	if done != nil {
		<-done
	}
	return nil
}

func (b *RedisNodeEventBus) consume(ctx context.Context, pubsub *redis.PubSub, handler UserEventHandler, done chan struct{}) {
	defer close(done)

	for {
		msg, err := pubsub.ReceiveMessage(ctx)
		if err != nil {
			return
		}

		event, err := parseNodeFrameEnvelope(msg.Payload)
		if err != nil {
			continue
		}

		handler(event)
	}
}

func marshalNodeFrameEnvelope(envelope nodeFrameEnvelope) (string, error) {
	payload, err := json.Marshal(envelope)
	if err != nil {
		return "", err
	}
	return string(payload), nil
}

func parseNodeFrameEnvelope(payload string) (UserNodeEvent, error) {
	var envelope nodeFrameEnvelope
	if err := json.Unmarshal([]byte(payload), &envelope); err != nil {
		return UserNodeEvent{}, err
	}
	if envelope.TargetUserID == "" || envelope.TargetSessionID == "" {
		return UserNodeEvent{}, fmt.Errorf("invalid node frame envelope")
	}
	if len(envelope.Frame) == 0 && !envelope.ResetMeeting && envelope.State == nil && !envelope.Close {
		return UserNodeEvent{}, fmt.Errorf("empty node frame envelope")
	}

	sessionID, err := strconv.ParseUint(envelope.TargetSessionID, 10, 64)
	if err != nil {
		return UserNodeEvent{}, err
	}
	return UserNodeEvent{
		TargetUserID:    envelope.TargetUserID,
		TargetSessionID: sessionID,
		Frame:           envelope.Frame,
		ResetMeeting:    envelope.ResetMeeting,
		State:           envelope.State,
		Close:           envelope.Close,
	}, nil
}

func nodeBroadcastChannel(nodeID string) string {
	return fmt.Sprintf("sig:node:%s", nodeID)
}
