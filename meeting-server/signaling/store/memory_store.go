package store

import (
	"errors"
	"fmt"
	"math/rand"
	"meeting-server/signaling/protocol"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
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
}

func NewMemoryStore() *MemoryStore {
	s := &MemoryStore{
		users:        make(map[string]User),
		userByID:     make(map[string]User),
		meetings:     make(map[string]*Meeting),
		participants: make(map[string]map[string]*protocol.Participant),
		joinOrder:    make(map[string]map[string]uint64),
		meetingMsgs:  make(map[string][]Message),
	}

	s.seedDefaultUsers()
	return s
}

func (s *MemoryStore) seedDefaultUsers() {
	defaults := []User{
		{ID: "u1001", Username: "demo", PasswordHash: "demo", DisplayName: "Demo User"},
		{ID: "u1002", Username: "alice", PasswordHash: "alice", DisplayName: "Alice"},
		{ID: "u1003", Username: "bob", PasswordHash: "bob", DisplayName: "Bob"},
	}

	for _, u := range defaults {
		s.users[u.Username] = u
		s.userByID[u.ID] = u
	}
}

func (s *MemoryStore) Authenticate(username, passwordHash string) (User, error) {
	s.mu.RLock()
	defer s.mu.RUnlock()

	user, ok := s.users[username]
	if !ok {
		return User{}, ErrUserNotFound
	}
	if user.PasswordHash != passwordHash {
		return User{}, ErrInvalidPassword
	}
	return user, nil
}

func (s *MemoryStore) GetUserByID(userID string) (User, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	u, ok := s.userByID[userID]
	return u, ok
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
