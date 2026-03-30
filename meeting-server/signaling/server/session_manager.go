package server

import (
	"meeting-server/signaling/protocol"
	"sync"

	"google.golang.org/protobuf/proto"
)

type SessionManager struct {
	mu       sync.RWMutex
	sessions map[uint64]*Session
	byUser   map[string]*Session
}

func NewSessionManager() *SessionManager {
	return &SessionManager{
		sessions: make(map[uint64]*Session),
		byUser:   make(map[string]*Session),
	}
}

func (m *SessionManager) Add(s *Session) {
	m.mu.Lock()
	m.sessions[s.ID] = s
	m.mu.Unlock()
}

func (m *SessionManager) Remove(s *Session) {
	m.mu.Lock()
	defer m.mu.Unlock()

	delete(m.sessions, s.ID)

	userID := s.UserID()
	if userID != "" {
		if current, ok := m.byUser[userID]; ok && current.ID == s.ID {
			delete(m.byUser, userID)
		}
	}
}

func (m *SessionManager) BindUser(s *Session, userID string) {
	var old *Session

	m.mu.Lock()
	if existing, ok := m.byUser[userID]; ok && existing.ID != s.ID {
		old = existing
	}
	m.byUser[userID] = s
	m.mu.Unlock()

	s.SetUserID(userID)

	if old != nil {
		_ = old.Send(protocol.AuthKickNotify, &protocol.AuthKickNotifyBody{Reason: "账号在其他设备登录"})
		old.Close()
	}
}

func (m *SessionManager) GetByUser(userID string) (*Session, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()

	s, ok := m.byUser[userID]
	return s, ok
}

func (m *SessionManager) BroadcastToRoom(roomID string, msgType protocol.SignalType, body proto.Message, excludeUserID string) {
	m.BroadcastToMeeting(roomID, msgType, body, excludeUserID)
}
func (m *SessionManager) BroadcastToMeeting(meetingID string, msgType protocol.SignalType, body proto.Message, excludeUserID string) {
	sessions := m.sessionsInMeeting(meetingID)
	for _, s := range sessions {
		if excludeUserID != "" && s.UserID() == excludeUserID {
			continue
		}
		_ = s.Send(msgType, body)
	}
}

func (m *SessionManager) sessionsInMeeting(meetingID string) []*Session {
	m.mu.RLock()
	defer m.mu.RUnlock()

	result := make([]*Session, 0)
	for _, s := range m.sessions {
		if s.MeetingID() == meetingID {
			result = append(result, s)
		}
	}
	return result
}
