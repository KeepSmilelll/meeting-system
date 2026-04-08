package server

import (
	"context"
	"meeting-server/signaling/protocol"
	"sync"

	"google.golang.org/protobuf/proto"
)

type MeetingFramePublisher interface {
	PublishMeetingFrame(ctx context.Context, meetingID string, frame []byte, excludeUserID string) error
}

type SessionManager struct {
	mu        sync.RWMutex
	sessions  map[uint64]*Session
	byUser    map[string]*Session
	publisher MeetingFramePublisher
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

func (m *SessionManager) SetMeetingFramePublisher(publisher MeetingFramePublisher) {
	m.mu.Lock()
	m.publisher = publisher
	m.mu.Unlock()
}

func (m *SessionManager) BroadcastToRoom(roomID string, msgType protocol.SignalType, body proto.Message, excludeUserID string) {
	m.BroadcastToMeeting(roomID, msgType, body, excludeUserID)
}
func (m *SessionManager) BroadcastToMeeting(meetingID string, msgType protocol.SignalType, body proto.Message, excludeUserID string) {
	frame, err := buildBroadcastFrame(msgType, body)
	if err != nil {
		return
	}

	m.DeliverMeetingFrame(meetingID, frame, excludeUserID)

	m.mu.RLock()
	publisher := m.publisher
	m.mu.RUnlock()
	if publisher != nil && meetingID != "" {
		_ = publisher.PublishMeetingFrame(context.Background(), meetingID, frame, excludeUserID)
	}
}

func (m *SessionManager) DeliverMeetingFrame(meetingID string, frame []byte, excludeUserID string) {
	sessions := m.sessionsInMeeting(meetingID)
	for _, s := range sessions {
		if excludeUserID != "" && s.UserID() == excludeUserID {
			continue
		}
		_ = s.SendRaw(frame)
	}
}

func (m *SessionManager) DeliverUserFrame(userID string, targetSessionID uint64, frame []byte) bool {
	if userID == "" || len(frame) == 0 {
		return false
	}

	target, ok := m.GetByUser(userID)
	if !ok || target == nil {
		return false
	}
	if targetSessionID != 0 && target.ID != targetSessionID {
		return false
	}

	return target.SendRaw(frame) == nil
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

func buildBroadcastFrame(msgType protocol.SignalType, body proto.Message) ([]byte, error) {
	var payload []byte
	if body != nil {
		b, err := proto.Marshal(body)
		if err != nil {
			return nil, err
		}
		payload = b
	}

	return protocol.EncodeFrame(msgType, payload), nil
}
