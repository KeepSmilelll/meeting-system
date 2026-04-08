package store

import (
	"context"
	"errors"
	"fmt"
	"time"

	"meeting-server/signaling/model"
	"meeting-server/signaling/protocol"

	"gorm.io/gorm"
)

// MeetingLifecycleStore keeps the meeting handler on a single interface so the
// handler can use either the existing in-memory path or the repo-backed path.
type MeetingLifecycleStore interface {
	CreateMeeting(title, password, hostUserID string, maxParticipants int) (*Meeting, *protocol.Participant, error)
	JoinMeeting(meetingID, password, userID string) (*Meeting, []*protocol.Participant, *protocol.Participant, error)
	LeaveMeeting(meetingID, userID string) (hostChanged bool, newHost *protocol.Participant, remaining int, err error)
	IsMeetingHost(meetingID, userID string) (bool, error)
	HasParticipant(meetingID, userID string) (bool, error)
	SnapshotMeeting(meetingID string) (*Meeting, []*protocol.Participant, error)
}

type meetingRepoAPI interface {
	FindByID(ctx context.Context, meetingID uint64) (*model.Meeting, error)
	FindByMeetingNo(ctx context.Context, meetingNo string) (*model.Meeting, error)
	CreateMeeting(ctx context.Context, meeting *model.Meeting) error
	TransferHost(ctx context.Context, meetingID uint64) (*model.Participant, error)
	DeleteMeeting(ctx context.Context, meetingID uint64) error
}

type participantRepoAPI interface {
	AddParticipant(ctx context.Context, participant *model.Participant) error
	MarkParticipantLeft(ctx context.Context, meetingID, userID uint64) error
	ListActiveParticipants(ctx context.Context, meetingID uint64) ([]model.Participant, error)
}

type repoMeetingStore struct {
	fallback        *MemoryStore
	meetingRepo     meetingRepoAPI
	participantRepo participantRepoAPI
}

func NewMeetingLifecycleStore(fallback *MemoryStore, meetingRepo meetingRepoAPI) MeetingLifecycleStore {
	return NewMeetingLifecycleStoreWithParticipantRepo(fallback, meetingRepo, nil)
}

func NewMeetingLifecycleStoreWithParticipantRepo(fallback *MemoryStore, meetingRepo meetingRepoAPI, participantRepo participantRepoAPI) MeetingLifecycleStore {
	if meetingRepo == nil {
		return fallback
	}
	if participantRepo == nil {
		if repo, ok := meetingRepo.(participantRepoAPI); ok {
			participantRepo = repo
		}
	}
	return &repoMeetingStore{
		fallback:        fallback,
		meetingRepo:     meetingRepo,
		participantRepo: participantRepo,
	}
}

func (s *repoMeetingStore) CreateMeeting(title, password, hostUserID string, maxParticipants int) (*Meeting, *protocol.Participant, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return nil, nil, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	if maxParticipants <= 0 {
		maxParticipants = 16
	}

	hostProfile, ok := s.lookupUser(hostUserID)
	if !ok {
		return nil, nil, ErrUserNotFound
	}

	hostNumericID, err := userIDToUint64(hostUserID)
	if err != nil {
		return nil, nil, fmt.Errorf("meeting lifecycle store: parse host id: %w", err)
	}

	meetingModel := &model.Meeting{
		MeetingNo:       s.nextMeetingNo(),
		Title:           title,
		HostUserID:      hostNumericID,
		PasswordHash:    password,
		Status:          0,
		MaxParticipants: maxParticipants,
	}
	if err := s.meetingRepo.CreateMeeting(context.Background(), meetingModel); err != nil {
		if s.fallback != nil && isInvalidDBError(err) {
			return s.fallback.CreateMeeting(title, password, hostUserID, maxParticipants)
		}
		return nil, nil, err
	}

	hostParticipant := &model.Participant{
		MeetingID:  meetingModel.ID,
		UserID:     hostNumericID,
		Role:       1,
		MediaState: 0,
		JoinedAt:   time.Now().UTC(),
	}
	if err := s.participantRepo.AddParticipant(context.Background(), hostParticipant); err != nil {
		if s.fallback != nil && isInvalidDBError(err) {
			return s.fallback.CreateMeeting(title, password, hostUserID, maxParticipants)
		}
		return nil, nil, err
	}

	return meetingFromModel(meetingModel), participantFromProfile(hostUserID, hostProfile, true), nil
}

func (s *repoMeetingStore) JoinMeeting(meetingID, password, userID string) (*Meeting, []*protocol.Participant, *protocol.Participant, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return nil, nil, nil, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	meetingModel, err := s.meetingRepo.FindByMeetingNo(context.Background(), meetingID)
	if err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.JoinMeeting(meetingID, password, userID)
		}
		return nil, nil, nil, err
	}

	meeting := meetingFromModel(meetingModel)
	if meeting.Password != "" && meeting.Password != password {
		return nil, nil, nil, ErrMeetingPassFailed
	}

	participants, err := s.participantRepo.ListActiveParticipants(context.Background(), meetingModel.ID)
	if err != nil {
		if s.fallback != nil && isInvalidDBError(err) {
			return s.fallback.JoinMeeting(meetingID, password, userID)
		}
		return nil, nil, nil, err
	}
	if len(participants) >= meeting.MaxParticipants {
		return nil, nil, nil, ErrMeetingFull
	}

	if existing := findParticipantByUserID(participants, userID, s.lookupUser); existing != nil {
		list, err := s.loadParticipants(meetingModel.ID)
		if err != nil {
			return nil, nil, nil, err
		}
		return meeting, list, existing, nil
	}

	profile, ok := s.lookupUser(userID)
	if !ok {
		return nil, nil, nil, ErrUserNotFound
	}

	internalUserID, err := userIDToUint64(userID)
	if err != nil {
		return nil, nil, nil, fmt.Errorf("meeting lifecycle store: parse user id: %w", err)
	}

	joined := &model.Participant{
		MeetingID:  meetingModel.ID,
		UserID:     internalUserID,
		Role:       0,
		MediaState: 0,
		JoinedAt:   time.Now().UTC(),
	}
	if err := s.participantRepo.AddParticipant(context.Background(), joined); err != nil {
		if s.fallback != nil && isInvalidDBError(err) {
			return s.fallback.JoinMeeting(meetingID, password, userID)
		}
		return nil, nil, nil, err
	}

	list, err := s.loadParticipants(meetingModel.ID)
	if err != nil {
		return nil, nil, nil, err
	}
	return meeting, list, participantFromProfile(userID, profile, false), nil
}

func (s *repoMeetingStore) LeaveMeeting(meetingID, userID string) (bool, *protocol.Participant, int, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return false, nil, 0, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	meetingModel, err := s.meetingRepo.FindByMeetingNo(context.Background(), meetingID)
	if err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.LeaveMeeting(meetingID, userID)
		}
		return false, nil, 0, err
	}

	internalUserID, err := userIDToUint64(userID)
	if err != nil {
		return false, nil, 0, fmt.Errorf("meeting lifecycle store: parse user id: %w", err)
	}

	if err := s.participantRepo.MarkParticipantLeft(context.Background(), meetingModel.ID, internalUserID); err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.LeaveMeeting(meetingID, userID)
		}
		return false, nil, 0, err
	}

	active, err := s.participantRepo.ListActiveParticipants(context.Background(), meetingModel.ID)
	if err != nil {
		return false, nil, 0, err
	}
	remaining := len(active)
	if remaining == 0 {
		if err := s.meetingRepo.DeleteMeeting(context.Background(), meetingModel.ID); err != nil {
			return false, nil, 0, err
		}
		return false, nil, 0, nil
	}

	if meetingModel.HostUserID != internalUserID {
		return false, nil, remaining, nil
	}

	newHost, err := s.meetingRepo.TransferHost(context.Background(), meetingModel.ID)
	if err != nil {
		return false, nil, 0, err
	}
	return true, s.participantFromModel(newHost), remaining, nil
}

func (s *repoMeetingStore) IsMeetingHost(meetingID, userID string) (bool, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return false, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	meetingModel, err := s.meetingRepo.FindByMeetingNo(context.Background(), meetingID)
	if err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.IsMeetingHost(meetingID, userID)
		}
		return false, err
	}

	internalUserID, err := userIDToUint64(userID)
	if err != nil {
		return false, err
	}
	return meetingModel.HostUserID == internalUserID, nil
}

func (s *repoMeetingStore) HasParticipant(meetingID, userID string) (bool, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return false, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	meetingModel, err := s.meetingRepo.FindByMeetingNo(context.Background(), meetingID)
	if err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.HasParticipant(meetingID, userID)
		}
		return false, err
	}

	active, err := s.participantRepo.ListActiveParticipants(context.Background(), meetingModel.ID)
	if err != nil {
		return false, err
	}
	return findParticipantByUserID(active, userID, s.lookupUser) != nil, nil
}

func (s *repoMeetingStore) SnapshotMeeting(meetingID string) (*Meeting, []*protocol.Participant, error) {
	if s == nil || s.meetingRepo == nil || s.participantRepo == nil {
		return nil, nil, fmt.Errorf("meeting lifecycle store: repo not configured: %w", gorm.ErrInvalidDB)
	}

	meetingModel, err := s.meetingRepo.FindByMeetingNo(context.Background(), meetingID)
	if err != nil {
		if s.fallback != nil && isNotFoundError(err) {
			return s.fallback.SnapshotMeeting(meetingID)
		}
		return nil, nil, err
	}

	participants, err := s.loadParticipants(meetingModel.ID)
	if err != nil {
		return nil, nil, err
	}

	return meetingFromModel(meetingModel), participants, nil
}

func (s *repoMeetingStore) lookupUser(userID string) (User, bool) {
	if s == nil || s.fallback == nil {
		return User{}, false
	}
	return s.fallback.GetUserByID(userID)
}

func (s *repoMeetingStore) loadParticipants(meetingID uint64) ([]*protocol.Participant, error) {
	participants, err := s.participantRepo.ListActiveParticipants(context.Background(), meetingID)
	if err != nil {
		return nil, err
	}

	result := make([]*protocol.Participant, 0, len(participants))
	for _, participant := range participants {
		profile, _ := s.lookupUser(userIDFromUint64(participant.UserID))
		result = append(result, participantFromProfile(userIDFromUint64(participant.UserID), profile, participant.Role == 1))
	}

	return result, nil
}

func (s *repoMeetingStore) participantFromModel(participant *model.Participant) *protocol.Participant {
	if participant == nil {
		return nil
	}

	profile, _ := s.lookupUser(userIDFromUint64(participant.UserID))
	return participantFromProfile(userIDFromUint64(participant.UserID), profile, participant.Role == 1)
}

func (s *repoMeetingStore) nextMeetingNo() string {
	if s != nil && s.fallback != nil {
		return s.fallback.generateMeetingIDLocked()
	}
	return fmt.Sprintf("%06d", time.Now().UnixNano()%1000000)
}

func participantFromProfile(userID string, profile User, isHost bool) *protocol.Participant {
	role := int32(0)
	if isHost {
		role = 1
	}

	result := &protocol.Participant{
		UserId:    userID,
		Role:      role,
		IsAudioOn: true,
		IsVideoOn: true,
		IsSharing: false,
	}
	if profile.ID != "" {
		result.DisplayName = profile.DisplayName
		result.AvatarUrl = profile.AvatarURL
	}
	if result.DisplayName == "" {
		result.DisplayName = userID
	}
	return result
}

func meetingFromModel(meeting *model.Meeting) *Meeting {
	if meeting == nil {
		return nil
	}
	return &Meeting{
		ID:              meeting.MeetingNo,
		Title:           meeting.Title,
		Password:        meeting.PasswordHash,
		HostUserID:      userIDFromUint64(meeting.HostUserID),
		MaxParticipants: meeting.MaxParticipants,
		CreatedAt:       meeting.CreatedAt,
	}
}

func findParticipantByUserID(participants []model.Participant, userID string, lookup func(string) (User, bool)) *protocol.Participant {
	internalUserID, err := userIDToUint64(userID)
	if err != nil {
		return nil
	}
	for _, participant := range participants {
		if participant.UserID == internalUserID {
			profile, _ := lookup(userID)
			return participantFromProfile(userID, profile, participant.Role == 1)
		}
	}
	return nil
}

func isInvalidDBError(err error) bool {
	return err != nil && (errors.Is(err, gorm.ErrInvalidDB) || errors.Is(err, gorm.ErrInvalidTransaction))
}

func isNotFoundError(err error) bool {
	return err != nil && errors.Is(err, gorm.ErrRecordNotFound)
}
