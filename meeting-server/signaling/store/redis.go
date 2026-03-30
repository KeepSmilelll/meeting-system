package store

import (
	"context"
	"fmt"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"strconv"
	"time"

	"github.com/redis/go-redis/v9"
)

type RedisRoomStore struct {
	enabled    bool
	client     *redis.Client
	ownsClient bool
}

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

func (r *RedisRoomStore) UpsertRoom(ctx context.Context, meetingID, hostUserID string) error {
	if !r.Enabled() {
		return nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	return r.client.HSet(ctx, key, map[string]any{
		"host_id":    hostUserID,
		"status":     "1",
		"created_at": strconv.FormatInt(time.Now().Unix(), 10),
	}).Err()
}

func (r *RedisRoomStore) TransferHost(ctx context.Context, meetingID, newHostID string) error {
	if !r.Enabled() {
		return nil
	}

	key := fmt.Sprintf("room:%s", meetingID)
	return r.client.HSet(ctx, key, "host_id", newHostID).Err()
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

func (r *RedisRoomStore) DeleteRoom(ctx context.Context, meetingID string) error {
	if !r.Enabled() {
		return nil
	}

	setKey := fmt.Sprintf("room_members:%s", meetingID)
	roomKey := fmt.Sprintf("room:%s", meetingID)

	members, err := r.client.SMembers(ctx, setKey).Result()
	if err != nil && err != redis.Nil {
		return err
	}

	pipe := r.client.TxPipeline()
	pipe.Del(ctx, setKey)
	pipe.Del(ctx, roomKey)
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
