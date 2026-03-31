package store

import (
	"context"
	"fmt"
	"time"

	"meeting-server/signaling/model"

	"gorm.io/gorm"
)

// MeetingRepo provides GORM-backed persistence for meeting lifecycle data.
// Host promotion follows the earliest active join time, with ID as the tie-breaker.
type MeetingRepo struct {
	db *gorm.DB
}

func NewMeetingRepo(db *gorm.DB) *MeetingRepo {
	return &MeetingRepo{db: db}
}

func (r *MeetingRepo) FindByID(ctx context.Context, meetingID uint64) (*model.Meeting, error) {
	if err := r.requireDB(); err != nil {
		return nil, err
	}

	var meeting model.Meeting
	err := r.db.WithContext(ctx).First(&meeting, "id = ?", meetingID).Error
	if err != nil {
		return nil, fmt.Errorf("meeting repo: find meeting by id %d: %w", meetingID, err)
	}

	return &meeting, nil
}

func (r *MeetingRepo) FindByMeetingNo(ctx context.Context, meetingNo string) (*model.Meeting, error) {
	if err := r.requireDB(); err != nil {
		return nil, err
	}
	if meetingNo == "" {
		return nil, fmt.Errorf("meeting repo: meeting_no is required: %w", gorm.ErrInvalidData)
	}

	var meeting model.Meeting
	err := r.db.WithContext(ctx).Where("meeting_no = ?", meetingNo).First(&meeting).Error
	if err != nil {
		return nil, fmt.Errorf("meeting repo: find meeting by meeting_no %q: %w", meetingNo, err)
	}

	return &meeting, nil
}

func (r *MeetingRepo) CreateMeeting(ctx context.Context, meeting *model.Meeting) error {
	if err := r.requireDB(); err != nil {
		return err
	}
	if meeting == nil {
		return fmt.Errorf("meeting repo: meeting payload is nil: %w", gorm.ErrInvalidData)
	}
	if meeting.MeetingNo == "" {
		return fmt.Errorf("meeting repo: meeting_no is required: %w", gorm.ErrInvalidData)
	}
	if meeting.Title == "" {
		return fmt.Errorf("meeting repo: title is required: %w", gorm.ErrInvalidData)
	}
	if meeting.MaxParticipants <= 0 {
		meeting.MaxParticipants = 16
	}

	if err := r.db.WithContext(ctx).Create(meeting).Error; err != nil {
		return fmt.Errorf("meeting repo: create meeting %q: %w", meeting.MeetingNo, err)
	}

	return nil
}

func (r *MeetingRepo) AddParticipant(ctx context.Context, participant *model.Participant) error {
	if err := r.requireDB(); err != nil {
		return err
	}
	if participant == nil {
		return fmt.Errorf("meeting repo: participant payload is nil: %w", gorm.ErrInvalidData)
	}
	if participant.MeetingID == 0 || participant.UserID == 0 {
		return fmt.Errorf("meeting repo: meeting_id and user_id are required: %w", gorm.ErrInvalidData)
	}

	if participant.JoinedAt.IsZero() {
		participant.JoinedAt = time.Now().UTC()
	}
	participant.LeftAt = nil

	if err := r.db.WithContext(ctx).Create(participant).Error; err != nil {
		return fmt.Errorf("meeting repo: add participant meeting=%d user=%d: %w", participant.MeetingID, participant.UserID, err)
	}

	return nil
}

func (r *MeetingRepo) MarkParticipantLeft(ctx context.Context, meetingID, userID uint64) error {
	if err := r.requireDB(); err != nil {
		return err
	}
	if meetingID == 0 || userID == 0 {
		return fmt.Errorf("meeting repo: meeting_id and user_id are required: %w", gorm.ErrInvalidData)
	}

	leftAt := time.Now().UTC()
	tx := r.db.WithContext(ctx).
		Model(&model.Participant{}).
		Where("meeting_id = ? AND user_id = ? AND left_at IS NULL", meetingID, userID).
		Updates(map[string]any{"left_at": leftAt})
	if tx.Error != nil {
		return fmt.Errorf("meeting repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, tx.Error)
	}
	if tx.RowsAffected == 0 {
		return fmt.Errorf("meeting repo: mark participant left meeting=%d user=%d: %w", meetingID, userID, gorm.ErrRecordNotFound)
	}

	return nil
}

func (r *MeetingRepo) ListActiveParticipants(ctx context.Context, meetingID uint64) ([]model.Participant, error) {
	if err := r.requireDB(); err != nil {
		return nil, err
	}
	if meetingID == 0 {
		return nil, fmt.Errorf("meeting repo: meeting_id is required: %w", gorm.ErrInvalidData)
	}

	participants := make([]model.Participant, 0)
	err := r.db.WithContext(ctx).
		Where("meeting_id = ? AND left_at IS NULL", meetingID).
		Order("joined_at ASC, id ASC").
		Find(&participants).Error
	if err != nil {
		return nil, fmt.Errorf("meeting repo: list active participants meeting=%d: %w", meetingID, err)
	}

	return participants, nil
}

// TransferHost promotes the earliest active participant in the meeting.
// Ties are broken by the smallest participant ID so the result is deterministic.
func (r *MeetingRepo) TransferHost(ctx context.Context, meetingID uint64) (*model.Participant, error) {
	if err := r.requireDB(); err != nil {
		return nil, err
	}
	if meetingID == 0 {
		return nil, fmt.Errorf("meeting repo: meeting_id is required: %w", gorm.ErrInvalidData)
	}

	tx := r.db.WithContext(ctx).Begin()
	if tx.Error != nil {
		return nil, fmt.Errorf("meeting repo: begin transfer host transaction: %w", tx.Error)
	}

	var meeting model.Meeting
	if err := tx.First(&meeting, "id = ?", meetingID).Error; err != nil {
		_ = tx.Rollback().Error
		return nil, fmt.Errorf("meeting repo: load meeting for transfer meeting=%d: %w", meetingID, err)
	}

	var participants []model.Participant
	if err := tx.Where("meeting_id = ? AND left_at IS NULL", meetingID).Order("joined_at ASC, id ASC").Find(&participants).Error; err != nil {
		_ = tx.Rollback().Error
		return nil, fmt.Errorf("meeting repo: load participants for transfer meeting=%d: %w", meetingID, err)
	}

	nextHost := pickNextHostCandidate(participants)
	if nextHost == nil {
		_ = tx.Rollback().Error
		return nil, fmt.Errorf("meeting repo: select next host meeting=%d: %w", meetingID, gorm.ErrRecordNotFound)
	}

	if err := tx.Model(&model.Meeting{}).
		Where("id = ?", meetingID).
		Updates(map[string]any{"host_user_id": nextHost.UserID}).Error; err != nil {
		_ = tx.Rollback().Error
		return nil, fmt.Errorf("meeting repo: update host meeting=%d: %w", meetingID, err)
	}

	if err := tx.Model(&model.Participant{}).
		Where("meeting_id = ? AND user_id = ? AND left_at IS NULL", meetingID, nextHost.UserID).
		Update("role", 1).Error; err != nil {
		_ = tx.Rollback().Error
		return nil, fmt.Errorf("meeting repo: promote participant meeting=%d user=%d: %w", meetingID, nextHost.UserID, err)
	}

	if err := tx.Commit().Error; err != nil {
		return nil, fmt.Errorf("meeting repo: commit transfer host meeting=%d: %w", meetingID, err)
	}

	nextHost.Role = 1
	return nextHost, nil
}

func (r *MeetingRepo) DeleteMeeting(ctx context.Context, meetingID uint64) error {
	if err := r.requireDB(); err != nil {
		return err
	}
	if meetingID == 0 {
		return fmt.Errorf("meeting repo: meeting_id is required: %w", gorm.ErrInvalidData)
	}

	tx := r.db.WithContext(ctx).Begin()
	if tx.Error != nil {
		return fmt.Errorf("meeting repo: begin delete meeting transaction: %w", tx.Error)
	}

	if err := tx.Where("meeting_id = ?", meetingID).Delete(&model.Participant{}).Error; err != nil {
		_ = tx.Rollback().Error
		return fmt.Errorf("meeting repo: delete participants meeting=%d: %w", meetingID, err)
	}
	if err := tx.Delete(&model.Meeting{}, meetingID).Error; err != nil {
		_ = tx.Rollback().Error
		return fmt.Errorf("meeting repo: delete meeting=%d: %w", meetingID, err)
	}

	if err := tx.Commit().Error; err != nil {
		return fmt.Errorf("meeting repo: commit delete meeting=%d: %w", meetingID, err)
	}
	return nil
}

func (r *MeetingRepo) requireDB() error {
	if r == nil || r.db == nil {
		return fmt.Errorf("meeting repo: invalid db: %w", gorm.ErrInvalidDB)
	}
	return nil
}

func pickNextHostCandidate(participants []model.Participant) *model.Participant {
	if len(participants) == 0 {
		return nil
	}

	best := participants[0]
	for i := 1; i < len(participants); i++ {
		candidate := participants[i]
		if candidate.JoinedAt.Before(best.JoinedAt) || (candidate.JoinedAt.Equal(best.JoinedAt) && candidate.ID < best.ID) {
			best = candidate
		}
	}

	return &best
}
