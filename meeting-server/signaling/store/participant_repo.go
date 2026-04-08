package store

import (
	"context"
	"fmt"
	"sort"
	"sync"
	"time"

	"meeting-server/signaling/model"

	"gorm.io/gorm"
)

// ParticipantRepo persists participant lifecycle records.
// Without a DB it falls back to the in-memory implementation used by unit tests.
type ParticipantRepo struct {
	db           *gorm.DB
	mu           sync.RWMutex
	nextID       uint64
	participants map[uint64]map[uint64]*model.Participant
}

func NewParticipantRepo(db *gorm.DB) *ParticipantRepo {
	return newParticipantRepo(db)
}

func NewInMemoryParticipantRepo() *ParticipantRepo {
	return newParticipantRepo(nil)
}

func newParticipantRepo(db *gorm.DB) *ParticipantRepo {
	return &ParticipantRepo{
		db:           db,
		nextID:       1,
		participants: make(map[uint64]map[uint64]*model.Participant),
	}
}

func (r *ParticipantRepo) AddParticipant(ctx context.Context, participant *model.Participant) error {
	if err := ctxErr(ctx); err != nil {
		return fmt.Errorf("participant repo: add participant: %w", err)
	}
	if err := r.requireRepo(); err != nil {
		return fmt.Errorf("participant repo: add participant: %w", err)
	}
	if participant == nil {
		return fmt.Errorf("participant repo: add participant: %w", errInvalidData)
	}
	if participant.MeetingID == 0 || participant.UserID == 0 {
		return fmt.Errorf("participant repo: add participant meeting=%d user=%d: %w", participant.MeetingID, participant.UserID, errInvalidData)
	}

	if r.db != nil {
		if participant.JoinedAt.IsZero() {
			participant.JoinedAt = time.Now().UTC()
		}
		participant.LeftAt = nil

		if err := r.db.WithContext(ctx).Create(participant).Error; err != nil {
			return fmt.Errorf("participant repo: add participant meeting=%d user=%d: %w", participant.MeetingID, participant.UserID, err)
		}
		return nil
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	bucket := r.ensureMeetingLocked(participant.MeetingID)
	if existing, ok := bucket[participant.UserID]; ok && existing.LeftAt == nil {
		return fmt.Errorf("participant repo: add participant meeting=%d user=%d: %w", participant.MeetingID, participant.UserID, errAlreadyExists)
	}

	cloned := cloneParticipantRecord(participant)
	if cloned.ID == 0 {
		cloned.ID = r.nextParticipantIDLocked()
	}
	if cloned.JoinedAt.IsZero() {
		cloned.JoinedAt = time.Now().UTC()
	}
	cloned.LeftAt = nil

	bucket[cloned.UserID] = cloned
	participant.ID = cloned.ID
	participant.JoinedAt = cloned.JoinedAt
	participant.LeftAt = nil
	return nil
}

func (r *ParticipantRepo) MarkParticipantLeft(ctx context.Context, meetingID, userID uint64) error {
	if err := ctxErr(ctx); err != nil {
		return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, err)
	}
	if err := r.requireRepo(); err != nil {
		return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, err)
	}
	if meetingID == 0 || userID == 0 {
		return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, errInvalidData)
	}

	if r.db != nil {
		leftAt := time.Now().UTC()
		tx := r.db.WithContext(ctx).
			Model(&model.Participant{}).
			Where("meeting_id = ? AND user_id = ? AND left_at IS NULL", meetingID, userID).
			Updates(map[string]any{"left_at": leftAt})
		if tx.Error != nil {
			return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, tx.Error)
		}
		if tx.RowsAffected == 0 {
			return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, gorm.ErrRecordNotFound)
		}
		return nil
	}

	r.mu.Lock()
	defer r.mu.Unlock()

	bucket, ok := r.participants[meetingID]
	if !ok {
		return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, errNotFound)
	}

	record, ok := bucket[userID]
	if !ok || record.LeftAt != nil {
		return fmt.Errorf("participant repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, errNotFound)
	}

	leftAt := time.Now().UTC()
	record.LeftAt = &leftAt
	return nil
}

func (r *ParticipantRepo) ListActiveParticipants(ctx context.Context, meetingID uint64) ([]model.Participant, error) {
	if err := ctxErr(ctx); err != nil {
		return nil, fmt.Errorf("participant repo: list active participants meeting=%d: %w", meetingID, err)
	}
	if err := r.requireRepo(); err != nil {
		return nil, fmt.Errorf("participant repo: list active participants meeting=%d: %w", meetingID, err)
	}
	if meetingID == 0 {
		return nil, fmt.Errorf("participant repo: list active participants meeting=%d: %w", meetingID, errInvalidData)
	}

	if r.db != nil {
		participants := make([]model.Participant, 0)
		err := r.db.WithContext(ctx).
			Where("meeting_id = ? AND left_at IS NULL", meetingID).
			Order("joined_at ASC, id ASC").
			Find(&participants).Error
		if err != nil {
			return nil, fmt.Errorf("participant repo: list active participants meeting=%d: %w", meetingID, err)
		}
		return participants, nil
	}

	r.mu.RLock()
	defer r.mu.RUnlock()

	bucket, ok := r.participants[meetingID]
	if !ok {
		return nil, nil
	}

	out := make([]model.Participant, 0, len(bucket))
	for _, record := range bucket {
		if record.LeftAt != nil {
			continue
		}
		out = append(out, *cloneParticipantRecord(record))
	}

	sort.Slice(out, func(i, j int) bool {
		if out[i].JoinedAt.Equal(out[j].JoinedAt) {
			return out[i].ID < out[j].ID
		}
		return out[i].JoinedAt.Before(out[j].JoinedAt)
	})

	return out, nil
}

func (r *ParticipantRepo) FindActiveParticipant(ctx context.Context, meetingID, userID uint64) (*model.Participant, error) {
	if err := ctxErr(ctx); err != nil {
		return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, err)
	}
	if err := r.requireRepo(); err != nil {
		return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, err)
	}
	if meetingID == 0 || userID == 0 {
		return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, errInvalidData)
	}

	if r.db != nil {
		var participant model.Participant
		err := r.db.WithContext(ctx).
			Where("meeting_id = ? AND user_id = ? AND left_at IS NULL", meetingID, userID).
			First(&participant).Error
		if err != nil {
			return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, err)
		}
		return cloneParticipantRecord(&participant), nil
	}

	r.mu.RLock()
	defer r.mu.RUnlock()

	bucket, ok := r.participants[meetingID]
	if !ok {
		return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, errNotFound)
	}

	record, ok := bucket[userID]
	if !ok || record.LeftAt != nil {
		return nil, fmt.Errorf("participant repo: find active participant meeting=%d user=%d: %w", meetingID, userID, errNotFound)
	}

	return cloneParticipantRecord(record), nil
}

func (r *ParticipantRepo) PickNextHostCandidate(ctx context.Context, meetingID uint64) (*model.Participant, error) {
	participants, err := r.ListActiveParticipants(ctx, meetingID)
	if err != nil {
		return nil, fmt.Errorf("participant repo: pick next host candidate meeting=%d: %w", meetingID, err)
	}

	candidate := pickNextHostCandidate(participants)
	if candidate == nil {
		return nil, fmt.Errorf("participant repo: pick next host candidate meeting=%d: %w", meetingID, errNotFound)
	}
	return candidate, nil
}

func (r *ParticipantRepo) requireRepo() error {
	if r == nil {
		return errInvalidRepo
	}
	if r.db != nil {
		return nil
	}
	if r.participants == nil {
		return errInvalidRepo
	}
	return nil
}

func (r *ParticipantRepo) ensureMeetingLocked(meetingID uint64) map[uint64]*model.Participant {
	bucket, ok := r.participants[meetingID]
	if !ok {
		bucket = make(map[uint64]*model.Participant)
		r.participants[meetingID] = bucket
	}
	return bucket
}

func (r *ParticipantRepo) nextParticipantIDLocked() uint64 {
	id := r.nextID
	r.nextID++
	return id
}

func cloneParticipantRecord(src *model.Participant) *model.Participant {
	if src == nil {
		return nil
	}

	cloned := *src
	if src.LeftAt != nil {
		leftAt := *src.LeftAt
		cloned.LeftAt = &leftAt
	}
	return &cloned
}

func ctxErr(ctx context.Context) error {
	if ctx == nil {
		return nil
	}
	return ctx.Err()
}

var (
	errInvalidRepo   = fmt.Errorf("invalid repo")
	errInvalidData   = fmt.Errorf("invalid data")
	errAlreadyExists = fmt.Errorf("participant already exists")
	errNotFound      = fmt.Errorf("participant not found")
)
