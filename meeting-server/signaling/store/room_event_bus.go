package store

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"sync"

	"meeting-server/signaling/config"

	"github.com/redis/go-redis/v9"
)

const roomBroadcastPattern = "sig:room:*"

type MeetingFrameHandler func(meetingID string, frame []byte, excludeUserID string)

type RedisRoomEventBus struct {
	enabled bool
	client  *redis.Client
	nodeID  string

	mu     sync.Mutex
	pubsub *redis.PubSub
	cancel context.CancelFunc
	done   chan struct{}
}

type roomBroadcastEnvelope struct {
	NodeID        string `json:"node_id"`
	ExcludeUserID string `json:"exclude_user_id,omitempty"`
	Frame         []byte `json:"frame"`
}

func NewRedisRoomEventBusWithClient(cfg config.Config, client *redis.Client) *RedisRoomEventBus {
	if client == nil || !cfg.EnableRedis {
		return &RedisRoomEventBus{}
	}

	return &RedisRoomEventBus{
		enabled: true,
		client:  client,
		nodeID:  cfg.NodeID,
	}
}

func (b *RedisRoomEventBus) Enabled() bool {
	return b != nil && b.enabled
}

func (b *RedisRoomEventBus) Start(ctx context.Context, handler MeetingFrameHandler) error {
	if !b.Enabled() || handler == nil {
		return nil
	}

	b.mu.Lock()
	defer b.mu.Unlock()
	if b.pubsub != nil {
		return nil
	}

	runCtx, cancel := context.WithCancel(ctx)
	pubsub := b.client.PSubscribe(runCtx, roomBroadcastPattern)
	if _, err := pubsub.Receive(runCtx); err != nil {
		cancel()
		_ = pubsub.Close()
		return fmt.Errorf("room event bus subscribe: %w", err)
	}

	done := make(chan struct{})
	b.pubsub = pubsub
	b.cancel = cancel
	b.done = done

	go b.consume(runCtx, pubsub, handler, done)
	return nil
}

func (b *RedisRoomEventBus) PublishMeetingFrame(ctx context.Context, meetingID string, frame []byte, excludeUserID string) error {
	if !b.Enabled() || meetingID == "" || len(frame) == 0 {
		return nil
	}

	payload, err := marshalRoomBroadcastEnvelope(roomBroadcastEnvelope{
		NodeID:        b.nodeID,
		ExcludeUserID: excludeUserID,
		Frame:         frame,
	})
	if err != nil {
		return fmt.Errorf("room event bus marshal: %w", err)
	}

	if err := b.client.Publish(ctx, roomBroadcastChannel(meetingID), payload).Err(); err != nil {
		return fmt.Errorf("room event bus publish: %w", err)
	}
	return nil
}

func (b *RedisRoomEventBus) Close() error {
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

func (b *RedisRoomEventBus) consume(ctx context.Context, pubsub *redis.PubSub, handler MeetingFrameHandler, done chan struct{}) {
	defer close(done)

	for {
		msg, err := pubsub.ReceiveMessage(ctx)
		if err != nil {
			return
		}

		meetingID, envelope, err := parseRoomBroadcastMessage(msg.Channel, msg.Payload)
		if err != nil {
			continue
		}
		if envelope.NodeID == b.nodeID {
			continue
		}

		handler(meetingID, envelope.Frame, envelope.ExcludeUserID)
	}
}

func marshalRoomBroadcastEnvelope(envelope roomBroadcastEnvelope) (string, error) {
	payload, err := json.Marshal(envelope)
	if err != nil {
		return "", err
	}
	return string(payload), nil
}

func parseRoomBroadcastMessage(channel string, payload string) (string, roomBroadcastEnvelope, error) {
	meetingID, err := parseMeetingIDFromChannel(channel)
	if err != nil {
		return "", roomBroadcastEnvelope{}, err
	}

	var envelope roomBroadcastEnvelope
	if err := json.Unmarshal([]byte(payload), &envelope); err != nil {
		return "", roomBroadcastEnvelope{}, err
	}
	if len(envelope.Frame) == 0 {
		return "", roomBroadcastEnvelope{}, fmt.Errorf("empty frame")
	}

	return meetingID, envelope, nil
}

func parseMeetingIDFromChannel(channel string) (string, error) {
	const prefix = "sig:room:"
	if !strings.HasPrefix(channel, prefix) {
		return "", fmt.Errorf("invalid room channel %q", channel)
	}

	meetingID := strings.TrimPrefix(channel, prefix)
	if meetingID == "" {
		return "", fmt.Errorf("empty meeting id")
	}
	return meetingID, nil
}

func roomBroadcastChannel(meetingID string) string {
	return fmt.Sprintf("sig:room:%s", meetingID)
}
