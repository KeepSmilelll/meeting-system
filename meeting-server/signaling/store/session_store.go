package store

import (
	"context"
	"fmt"
	"strconv"
	"time"

	"meeting-server/signaling/config"

	"github.com/redis/go-redis/v9"
)

type SessionPresence struct {
	UserID    string
	NodeID    string
	SessionID uint64
	MeetingID string
	Status    int32
}

type SessionStore interface {
	Upsert(ctx context.Context, presence SessionPresence, ttl time.Duration) error
	Get(ctx context.Context, userID string) (*SessionPresence, error)
	Delete(ctx context.Context, userID string, sessionID uint64) error
}

type RedisSessionStore struct {
	enabled bool
	client  *redis.Client
}

var compareDeleteSessionScript = redis.NewScript(`
local current = redis.call("HGET", KEYS[1], "session_id")
if current == false then
	return 0
end
if current == ARGV[1] then
	return redis.call("DEL", KEYS[1])
end
return 0
`)

func NewRedisSessionStoreWithClient(cfg config.Config, client *redis.Client) *RedisSessionStore {
	if client == nil || !cfg.EnableRedis {
		return &RedisSessionStore{}
	}

	return &RedisSessionStore{
		enabled: true,
		client:  client,
	}
}

func (s *RedisSessionStore) Enabled() bool {
	return s != nil && s.enabled
}

func (s *RedisSessionStore) Upsert(ctx context.Context, presence SessionPresence, ttl time.Duration) error {
	if !s.Enabled() || presence.UserID == "" || presence.NodeID == "" || presence.SessionID == 0 {
		return nil
	}
	if ttl <= 0 {
		ttl = 24 * time.Hour
	}

	key := sessionKey(presence.UserID)
	pipe := s.client.TxPipeline()
	pipe.HSet(ctx, key, map[string]any{
		"node_id":      presence.NodeID,
		"session_id":   strconv.FormatUint(presence.SessionID, 10),
		"meeting_id":   presence.MeetingID,
		"status":       strconv.FormatInt(int64(presence.Status), 10),
		"connected_at": strconv.FormatInt(time.Now().Unix(), 10),
	})
	pipe.Expire(ctx, key, ttl)

	_, err := pipe.Exec(ctx)
	if err != nil {
		return fmt.Errorf("session store upsert: %w", err)
	}
	return nil
}

func (s *RedisSessionStore) Get(ctx context.Context, userID string) (*SessionPresence, error) {
	if !s.Enabled() || userID == "" {
		return nil, nil
	}

	values, err := s.client.HGetAll(ctx, sessionKey(userID)).Result()
	if err != nil {
		return nil, fmt.Errorf("session store get: %w", err)
	}
	if len(values) == 0 {
		return nil, nil
	}

	sessionID, err := strconv.ParseUint(values["session_id"], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("session store parse session_id: %w", err)
	}
	status, err := strconv.ParseInt(values["status"], 10, 32)
	if err != nil {
		return nil, fmt.Errorf("session store parse status: %w", err)
	}

	return &SessionPresence{
		UserID:    userID,
		NodeID:    values["node_id"],
		SessionID: sessionID,
		MeetingID: values["meeting_id"],
		Status:    int32(status),
	}, nil
}

func (s *RedisSessionStore) Delete(ctx context.Context, userID string, sessionID uint64) error {
	if !s.Enabled() || userID == "" || sessionID == 0 {
		return nil
	}

	if err := compareDeleteSessionScript.Run(ctx, s.client, []string{sessionKey(userID)}, strconv.FormatUint(sessionID, 10)).Err(); err != nil {
		return fmt.Errorf("session store delete: %w", err)
	}
	return nil
}

func sessionKey(userID string) string {
	return fmt.Sprintf("session:%s", userID)
}
