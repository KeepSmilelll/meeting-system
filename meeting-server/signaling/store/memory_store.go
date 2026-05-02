package store

import (
	"context"
	"errors"
	"fmt"
	"math/rand"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/model"
	"meeting-server/signaling/protocol"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"gorm.io/gorm"
)

var (
	ErrUserNotFound      = errors.New("user not found")
	ErrInvalidPassword   = errors.New("invalid password")
	ErrMeetingNotFound   = errors.New("meeting not found")
	ErrMeetingFull       = errors.New("meeting is full")
	ErrMeetingPassFailed = errors.New("meeting password mismatch")
)

type User struct {
	ID           string
	Username     string
	PasswordHash string
	DisplayName  string
	AvatarURL    string
}

type Meeting struct {
	ID              string
	Title           string
	Password        string
	HostUserID      string
	MaxParticipants int
	CreatedAt       time.Time
}

type Message struct {
	ID        string
	MeetingID string
	SenderID  string
	Type      int32
	Content   string
	ReplyToID string
	Timestamp int64
}

type UserAuthRepo interface {
	FindByUsername(ctx context.Context, username string) (*model.User, error)
	FindByID(ctx context.Context, userID uint64) (*model.User, error)
	Create(ctx context.Context, user *model.User) error
}

type MemoryStore struct {
	mu sync.RWMutex

	users        map[string]User
	userByID     map[string]User
	meetings     map[string]*Meeting
	participants map[string]map[string]*protocol.Participant // meetingID -> userID
	joinOrder    map[string]map[string]uint64                // meetingID -> userID -> join sequence
	meetingMsgs  map[string][]Message
	meetingSeq   atomic.Uint64
	messageSeq   atomic.Uint64
	joinSeq      atomic.Uint64

	passwordHasher *auth.PasswordHasher
	userRepo       UserAuthRepo
}

func NewMemoryStore() *MemoryStore {
	s := &MemoryStore{
		users:          make(map[string]User),
		userByID:       make(map[string]User),
		meetings:       make(map[string]*Meeting),
		participants:   make(map[string]map[string]*protocol.Participant),
		joinOrder:      make(map[string]map[string]uint64),
		meetingMsgs:    make(map[string][]Message),
		passwordHasher: auth.NewPasswordHasher(),
	}

	s.seedDefaultUsers()
	return s
}

func (s *MemoryStore) SetUserRepo(repo UserAuthRepo) {
	s.mu.Lock()
	s.userRepo = repo
	s.mu.Unlock()
}

func (s *MemoryStore) SeedDefaultUsersToRepo(ctx context.Context) error {
	s.mu.RLock()
	repo := s.userRepo
	defaults := make([]User, 0, len(s.users))
	for _, user := range s.users {
		defaults = append(defaults, user)
	}
	s.mu.RUnlock()

	if repo == nil {
		return nil
	}

	for _, localUser := range defaults {
		_, err := repo.FindByUsername(ctx, localUser.Username)
		if err == nil {
			continue
		}
		if !errors.Is(err, gorm.ErrRecordNotFound) {
			if errors.Is(err, gorm.ErrInvalidDB) || errors.Is(err, gorm.ErrInvalidTransaction) {
				return nil
			}
			return fmt.Errorf("seed default user %s: %w", localUser.Username, err)
		}

		id, err := userIDToUint64(localUser.ID)
		if err != nil {
			return fmt.Errorf("seed default user %s: parse id: %w", localUser.Username, err)
		}

		createErr := repo.Create(ctx, &model.User{
			ID:           id,
			Username:     localUser.Username,
			DisplayName:  localUser.DisplayName,
			AvatarURL:    localUser.AvatarURL,
			PasswordHash: localUser.PasswordHash,
			Status:       0,
		})
		if createErr != nil {
			return fmt.Errorf("seed default user %s: %w", localUser.Username, createErr)
		}
	}

	return nil
}

func (s *MemoryStore) seedDefaultUsers() {
	defaults := []struct {
		ID          string
		Username    string
		PasswordRaw string
		DisplayName string
	}{
		{ID: "u1001", Username: "demo", PasswordRaw: "demo", DisplayName: "Demo User"},
		{ID: "u1002", Username: "alice", PasswordRaw: "alice", DisplayName: "Alice"},
		{ID: "u1003", Username: "bob", PasswordRaw: "bob", DisplayName: "Bob"},
	}

	for _, raw := range defaults {
		passwordHash := raw.PasswordRaw
		if s.passwordHasher != nil {
			if hashed, err := s.passwordHasher.HashPassword(raw.PasswordRaw); err == nil {
				passwordHash = hashed
			}
		}

		u := User{
			ID:           raw.ID,
			Username:     raw.Username,
			PasswordHash: passwordHash,
			DisplayName:  raw.DisplayName,
		}
		s.users[u.Username] = u
		s.userByID[u.ID] = u
	}
}

func (s *MemoryStore) Authenticate(username, passwordHash string) (User, error) {
	s.mu.RLock()
	repo := s.userRepo
	localUser, localUserExists := s.users[username]
	s.mu.RUnlock()

	if repo != nil {
		repoUser, found, err := s.loadUserByUsernameFromRepo(repo, username)
		if err != nil {
			if !s.canFallbackToMemory(err) {
				return User{}, err
			}
		} else if found {
			if s.verifyPassword(repoUser.PasswordHash, passwordHash) {
				return repoUser, nil
			}
			return User{}, ErrInvalidPassword
		}
	}

	if !localUserExists {
		return User{}, ErrUserNotFound
	}
	if !s.verifyPassword(localUser.PasswordHash, passwordHash) {
		return User{}, ErrInvalidPassword
	}
	return localUser, nil
}

func (s *MemoryStore) GetUserByID(userID string) (User, bool) {
	s.mu.RLock()
	repo := s.userRepo
	localUser, localExists := s.userByID[userID]
	s.mu.RUnlock()

	if repo != nil {
		repoUser, found, err := s.loadUserByIDFromRepo(repo, userID)
		if err != nil {
			if !s.canFallbackToMemory(err) {
				return User{}, false
			}
		} else if found {
			return repoUser, true
		}
	}

	if !localExists {
		return User{}, false
	}
	return localUser, true
}

func (s *MemoryStore) verifyPassword(storedHash, provided string) bool {
	if s.passwordHasher == nil || !auth.IsArgon2idHash(storedHash) {
		return false
	}

	return s.passwordHasher.VerifyPassword(storedHash, provided) == nil
}

func (s *MemoryStore) canFallbackToMemory(err error) bool {
	return errors.Is(err, gorm.ErrRecordNotFound) ||
		errors.Is(err, gorm.ErrInvalidDB) ||
		errors.Is(err, gorm.ErrInvalidTransaction)
}

func (s *MemoryStore) loadUserByUsernameFromRepo(repo UserAuthRepo, username string) (User, bool, error) {
	record, err := repo.FindByUsername(context.Background(), username)
	if err != nil {
		return User{}, false, err
	}
	if record == nil {
		return User{}, false, nil
	}

	displayName := record.DisplayName
	if displayName == "" {
		displayName = record.Username
	}
	return User{
		ID:           userIDFromUint64(record.ID),
		Username:     record.Username,
		PasswordHash: record.PasswordHash,
		DisplayName:  displayName,
		AvatarURL:    record.AvatarURL,
	}, true, nil
}

func (s *MemoryStore) loadUserByIDFromRepo(repo UserAuthRepo, userID string) (User, bool, error) {
	numericUserID, err := userIDToUint64(userID)
	if err != nil {
		return User{}, false, err
	}

	record, findErr := repo.FindByID(context.Background(), numericUserID)
	if findErr != nil {
		return User{}, false, findErr
	}
	if record == nil {
		return User{}, false, nil
	}

	displayName := record.DisplayName
	if displayName == "" {
		displayName = record.Username
	}
	return User{
		ID:           userIDFromUint64(record.ID),
		Username:     record.Username,
		PasswordHash: record.PasswordHash,
		DisplayName:  displayName,
		AvatarURL:    record.AvatarURL,
	}, true, nil
}

func (s *MemoryStore) CreateMeeting(title, password, hostUserID string, maxParticipants int) (*Meeting, *protocol.Participant, error) {
	if maxParticipants <= 0 {
		maxParticipants = 16
	}

	s.mu.Lock()
	defer s.mu.Unlock()

	meetingID := s.generateMeetingIDLocked()
	meeting := &Meeting{
		ID:              meetingID,
		Title:           title,
		Password:        password,
		HostUserID:      hostUserID,
		MaxParticipants: maxParticipants,
		CreatedAt:       time.Now(),
	}

	host, ok := s.userByID[hostUserID]
	if !ok {
		return nil, nil, ErrUserNotFound
	}

	hostParticipant := &protocol.Participant{
		UserId:      host.ID,
		DisplayName: host.DisplayName,
		AvatarUrl:   host.AvatarURL,
		Role:        1,
		IsAudioOn:   true,
		IsVideoOn:   true,
		IsSharing:   false,
	}

	s.meetings[meetingID] = meeting
	s.participants[meetingID] = map[string]*protocol.Participant{hostUserID: hostParticipant}
	s.joinOrder[meetingID] = map[string]uint64{hostUserID: s.joinSeq.Add(1)}

	return meeting, hostParticipant, nil
}

func (s *MemoryStore) JoinMeeting(meetingID, password, userID string) (*Meeting, []*protocol.Participant, *protocol.Participant, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	meeting, ok := s.meetings[meetingID]
	if !ok {
		return nil, nil, nil, ErrMeetingNotFound
	}

	if meeting.Password != "" && meeting.Password != password {
		return nil, nil, nil, ErrMeetingPassFailed
	}

	members := s.participants[meetingID]
	if len(members) >= meeting.MaxParticipants {
		return nil, nil, nil, ErrMeetingFull
	}

	if participant, exists := members[userID]; exists {
		list := participantsToList(members)
		return meeting, list, participant, nil
	}

	user, ok := s.userByID[userID]
	if !ok {
		return nil, nil, nil, ErrUserNotFound
	}

	participant := &protocol.Participant{
		UserId:      userID,
		DisplayName: user.DisplayName,
		AvatarUrl:   user.AvatarURL,
		Role:        0,
		IsAudioOn:   true,
		IsVideoOn:   true,
		IsSharing:   false,
	}
	members[userID] = participant
	if _, ok := s.joinOrder[meetingID]; !ok {
		s.joinOrder[meetingID] = make(map[string]uint64)
	}
	s.joinOrder[meetingID][userID] = s.joinSeq.Add(1)

	list := participantsToList(members)
	return meeting, list, participant, nil
}

func (s *MemoryStore) LeaveMeeting(meetingID, userID string) (hostChanged bool, newHost *protocol.Participant, remaining int, err error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	meeting, ok := s.meetings[meetingID]
	if !ok {
		return false, nil, 0, ErrMeetingNotFound
	}

	members := s.participants[meetingID]
	joinOrder := s.joinOrder[meetingID]
	delete(members, userID)
	delete(joinOrder, userID)

	if len(members) == 0 {
		delete(s.participants, meetingID)
		delete(s.joinOrder, meetingID)
		delete(s.meetings, meetingID)
		delete(s.meetingMsgs, meetingID)
		return false, nil, 0, nil
	}

	if meeting.HostUserID == userID {
		nextHostID, nextHost := pickNextHostParticipant(members, joinOrder)
		nextHost.Role = 1
		meeting.HostUserID = nextHostID
		return true, nextHost, len(members), nil
	}

	return false, nil, len(members), nil
}

func (s *MemoryStore) Participants(meetingID string) ([]*protocol.Participant, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	members, ok := s.participants[meetingID]
	if !ok {
		return nil, ErrMeetingNotFound
	}
	return participantsToList(members), nil
}

func (s *MemoryStore) IsMeetingHost(meetingID, userID string) (bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	meeting, ok := s.meetings[meetingID]
	if !ok {
		return false, ErrMeetingNotFound
	}
	return meeting.HostUserID == userID, nil
}

func (s *MemoryStore) HasParticipant(meetingID, userID string) (bool, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	members, ok := s.participants[meetingID]
	if !ok {
		return false, ErrMeetingNotFound
	}
	_, exists := members[userID]
	return exists, nil
}

func (s *MemoryStore) SetParticipantMediaSsrc(meetingID, userID string, audioSsrc, videoSsrc uint32) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	members, ok := s.participants[meetingID]
	if !ok {
		return ErrMeetingNotFound
	}
	participant, ok := members[userID]
	if !ok || participant == nil {
		return ErrUserNotFound
	}
	if audioSsrc != 0 {
		participant.AudioSsrc = audioSsrc
	}
	if videoSsrc != 0 {
		participant.VideoSsrc = videoSsrc
	}
	return nil
}

func (s *MemoryStore) SetParticipantMediaMuted(meetingID, userID string, mediaType int32, muted bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	members, ok := s.participants[meetingID]
	if !ok {
		return ErrMeetingNotFound
	}
	participant, ok := members[userID]
	if !ok || participant == nil {
		return ErrUserNotFound
	}

	switch mediaType {
	case 0:
		participant.IsAudioOn = !muted
	case 1:
		participant.IsVideoOn = !muted
	default:
		return nil
	}
	return nil
}

func (s *MemoryStore) SetParticipantSharing(meetingID, userID string, sharing bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	members, ok := s.participants[meetingID]
	if !ok {
		return ErrMeetingNotFound
	}
	participant, ok := members[userID]
	if !ok || participant == nil {
		return ErrUserNotFound
	}

	participant.IsSharing = sharing
	return nil
}

func (s *MemoryStore) SnapshotMeeting(meetingID string) (*Meeting, []*protocol.Participant, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	meeting, ok := s.meetings[meetingID]
	if !ok {
		return nil, nil, ErrMeetingNotFound
	}

	members, ok := s.participants[meetingID]
	if !ok {
		return nil, nil, ErrMeetingNotFound
	}

	meetingCopy := *meeting
	return &meetingCopy, participantsToList(members), nil
}

func (s *MemoryStore) SaveMessage(meetingID, senderID string, msgType int32, content, replyToID string) Message {
	id := s.messageSeq.Add(1)
	msg := Message{
		ID:        fmt.Sprintf("m%d", id),
		MeetingID: meetingID,
		SenderID:  senderID,
		Type:      msgType,
		Content:   content,
		ReplyToID: replyToID,
		Timestamp: time.Now().UnixMilli(),
	}

	s.mu.Lock()
	s.meetingMsgs[meetingID] = append(s.meetingMsgs[meetingID], msg)
	s.mu.Unlock()

	return msg
}

func (s *MemoryStore) ListMessages(meetingID string, limit int, beforeTimestampMs ...int64) []Message {
	if limit <= 0 {
		limit = 100
	}
	before := int64(0)
	if len(beforeTimestampMs) > 0 {
		before = beforeTimestampMs[0]
	}

	s.mu.RLock()
	defer s.mu.RUnlock()

	messages := s.meetingMsgs[meetingID]
	if len(messages) == 0 {
		return nil
	}

	filtered := messages
	if before > 0 {
		filtered = make([]Message, 0, len(messages))
		for _, message := range messages {
			if message.Timestamp < before {
				filtered = append(filtered, message)
			}
		}
	}
	if len(filtered) == 0 {
		return nil
	}

	start := 0
	if len(filtered) > limit {
		start = len(filtered) - limit
	}

	out := make([]Message, 0, len(filtered)-start)
	for i := start; i < len(filtered); i++ {
		out = append(out, filtered[i])
	}
	return out
}

func participantsToList(m map[string]*protocol.Participant) []*protocol.Participant {
	result := make([]*protocol.Participant, 0, len(m))
	for _, p := range m {
		result = append(result, p)
	}
	return result
}

func pickNextHostParticipant(members map[string]*protocol.Participant, joinOrder map[string]uint64) (string, *protocol.Participant) {
	var (
		bestUserID string
		bestSeq    uint64
		best       *protocol.Participant
	)

	for userID, participant := range members {
		seq := joinOrder[userID]
		if best == nil || seq < bestSeq || (seq == bestSeq && userID < bestUserID) {
			bestUserID = userID
			bestSeq = seq
			best = participant
		}
	}

	return bestUserID, best
}

func (s *MemoryStore) generateMeetingIDLocked() string {
	now := s.meetingSeq.Add(1)
	seed := rand.Intn(9000) + 1000
	raw := int(now%10000) + seed
	return strconv.Itoa(100000 + raw%900000)
}
