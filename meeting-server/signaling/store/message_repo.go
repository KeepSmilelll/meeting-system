package store

import (
	"context"
	"fmt"
	"sort"
	"strings"
	"sync"
	"time"

	"meeting-server/signaling/model"

	"gorm.io/gorm"
)

// MessageRepo stores chat messages for a meeting. It supports both
// GORM-backed persistence and an in-memory fallback for tests/local mode.
type MessageRepo struct {
	db *gorm.DB

	mu      sync.RWMutex
	nextID  uint64
	storage map[uint64][]model.Message
}

func NewMessageRepo(db *gorm.DB) *MessageRepo {
	return newMessageRepo(db)
}

func NewInMemoryMessageRepo() *MessageRepo {
	return newMessageRepo(nil)
}

func newMessageRepo(db *gorm.DB) *MessageRepo {
	return &MessageRepo{
		db:      db,
		nextID:  1,
		storage: make(map[uint64][]model.Message),
	}
}

func (r *MessageRepo) Save(ctx context.Context, message *model.Message) error {
	if ctx != nil {
		if err := ctx.Err(); err != nil {
			return fmt.Errorf("message repo: save canceled: %w", err)
		}
	}
	if r == nil {
		return fmt.Errorf("message repo: nil repo: %w", gorm.ErrInvalidDB)
	}
	if message == nil || message.MeetingID == 0 || message.SenderID == 0 || strings.TrimSpace(message.Content) == "" {
		return fmt.Errorf("message repo: invalid message payload: %w", gorm.ErrInvalidData)
	}

	if r.db != nil {
		if message.CreatedAt.IsZero() {
			message.CreatedAt = time.Now().UTC()
		}
		if err := r.db.WithContext(ctx).Create(message).Error; err != nil {
			return fmt.Errorf("message repo: save meeting=%d sender=%d: %w", message.MeetingID, message.SenderID, err)
		}
		return nil
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	cloned := *message
	if cloned.ID == 0 {
		cloned.ID = r.nextID
		r.nextID++
	}
	if cloned.CreatedAt.IsZero() {
		cloned.CreatedAt = time.Now().UTC()
	}

	r.storage[cloned.MeetingID] = append(r.storage[cloned.MeetingID], cloned)
	message.ID = cloned.ID
	message.CreatedAt = cloned.CreatedAt
	return nil
}

func (r *MessageRepo) ListByMeeting(ctx context.Context, meetingID uint64, limit int) ([]model.Message, error) {
	if ctx != nil {
		if err := ctx.Err(); err != nil {
			return nil, fmt.Errorf("message repo: list canceled: %w", err)
		}
	}
	if r == nil {
		return nil, fmt.Errorf("message repo: nil repo: %w", gorm.ErrInvalidDB)
	}
	if meetingID == 0 {
		return nil, fmt.Errorf("message repo: meeting id required: %w", gorm.ErrInvalidData)
	}

	if limit <= 0 {
		limit = 100
	}

	if r.db != nil {
		out := make([]model.Message, 0, limit)
		err := r.db.WithContext(ctx).
			Where("meeting_id = ?", meetingID).
			Order("created_at DESC, id DESC").
			Limit(limit).
			Find(&out).Error
		if err != nil {
			return nil, fmt.Errorf("message repo: list meeting=%d: %w", meetingID, err)
		}
		sort.Slice(out, func(i, j int) bool {
			if out[i].CreatedAt.Equal(out[j].CreatedAt) {
				return out[i].ID < out[j].ID
			}
			return out[i].CreatedAt.Before(out[j].CreatedAt)
		})
		return out, nil
	}

	r.mu.RLock()
	defer r.mu.RUnlock()

	messages := r.storage[meetingID]
	if len(messages) == 0 {
		return nil, nil
	}

	start := 0
	if len(messages) > limit {
		start = len(messages) - limit
	}

	out := make([]model.Message, 0, len(messages)-start)
	for i := start; i < len(messages); i++ {
		out = append(out, messages[i])
	}
	return out, nil
}

func (r *MessageRepo) SearchByContent(ctx context.Context, meetingID uint64, keyword string, limit int) ([]model.Message, error) {
	keyword = strings.TrimSpace(keyword)
	if keyword == "" {
		return nil, nil
	}

	list, err := r.ListByMeeting(ctx, meetingID, limit)
	if err != nil {
		return nil, err
	}

	lowerKeyword := strings.ToLower(keyword)
	out := make([]model.Message, 0, len(list))
	for _, item := range list {
		if strings.Contains(strings.ToLower(item.Content), lowerKeyword) {
			out = append(out, item)
		}
	}
	return out, nil
}
