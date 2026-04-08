package handler

import (
	"context"
	"errors"
	"io"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/model"
	"meeting-server/signaling/protocol"
	pb "meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"
	"net"
	"sort"
	"sync"
	"testing"
	"time"

	"github.com/alicebob/miniredis/v2"
	"google.golang.org/protobuf/proto"
)

func newRunningSession(t *testing.T, id uint64, cfg config.Config) (*server.Session, net.Conn, func()) {
	t.Helper()

	srv, cli := net.Pipe()
	sess := server.NewSession(id, srv, cfg)
	sess.Start(func(*server.Session, protocol.SignalType, []byte) {}, func(*server.Session) {})

	cleanup := func() {
		sess.Close()
		_ = srv.Close()
		_ = cli.Close()
	}

	return sess, cli, cleanup
}

func readFrame(t *testing.T, conn net.Conn, maxPayload uint32) (protocol.SignalType, []byte) {
	t.Helper()

	header := make([]byte, protocol.HeaderSize)
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("read header failed: %v", err)
	}

	msgType, payloadLen, err := protocol.DecodeHeader(header, maxPayload)
	if err != nil {
		t.Fatalf("decode header failed: %v", err)
	}

	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(conn, payload); err != nil {
			t.Fatalf("read payload failed: %v", err)
		}
	}

	return msgType, payload
}

func readMeetStateSync(t *testing.T, conn net.Conn, maxPayload uint32) protocol.MeetStateSyncNotifyBody {
	t.Helper()

	msgType, payload := readFrame(t, conn, maxPayload)
	if msgType != protocol.MeetStateSync {
		t.Fatalf("unexpected state sync type: got %v want %v", msgType, protocol.MeetStateSync)
	}

	var notify protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(payload, &notify); err != nil {
		t.Fatalf("unmarshal state sync failed: %v", err)
	}
	return notify
}

func participantIDs(participants []*protocol.Participant) []string {
	out := make([]string, 0, len(participants))
	for _, participant := range participants {
		if participant == nil {
			continue
		}
		out = append(out, participant.UserId)
	}
	sort.Strings(out)
	return out
}

type failingLeaveMeetingStore struct {
	err error
}

type configurableMeetingStore struct {
	createMeeting   func(title, password, hostUserID string, maxParticipants int) (*store.Meeting, *protocol.Participant, error)
	joinMeeting     func(meetingID, password, userID string) (*store.Meeting, []*protocol.Participant, *protocol.Participant, error)
	leaveMeeting    func(meetingID, userID string) (bool, *protocol.Participant, int, error)
	isMeetingHost   func(meetingID, userID string) (bool, error)
	hasParticipant  func(meetingID, userID string) (bool, error)
	snapshotMeeting func(meetingID string) (*store.Meeting, []*protocol.Participant, error)
}

type recordedDelete struct {
	userID    string
	sessionID uint64
}

type recordingSessionStore struct {
	mu      sync.Mutex
	entries map[string]store.SessionPresence
	upserts []store.SessionPresence
	deletes []recordedDelete
}

type recordingUserEventPublisher struct {
	mu       sync.Mutex
	frames   []store.UserNodeEvent
	controls []store.UserNodeEvent
}

func newRecordingSessionStore() *recordingSessionStore {
	return &recordingSessionStore{
		entries: make(map[string]store.SessionPresence),
	}
}

func (s *recordingSessionStore) Upsert(_ context.Context, presence store.SessionPresence, _ time.Duration) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.entries[presence.UserID] = presence
	s.upserts = append(s.upserts, presence)
	return nil
}

func (s *recordingSessionStore) Get(_ context.Context, userID string) (*store.SessionPresence, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	presence, ok := s.entries[userID]
	if !ok {
		return nil, nil
	}
	copy := presence
	return &copy, nil
}

func (s *recordingSessionStore) Delete(_ context.Context, userID string, sessionID uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	s.deletes = append(s.deletes, recordedDelete{userID: userID, sessionID: sessionID})
	if current, ok := s.entries[userID]; ok && current.SessionID == sessionID {
		delete(s.entries, userID)
	}
	return nil
}

func (p *recordingUserEventPublisher) PublishUserFrame(_ context.Context, nodeID, userID string, targetSessionID uint64, frame []byte) error {
	p.mu.Lock()
	defer p.mu.Unlock()

	p.frames = append(p.frames, store.UserNodeEvent{
		TargetNodeID:    nodeID,
		TargetUserID:    userID,
		TargetSessionID: targetSessionID,
		Frame:           append([]byte(nil), frame...),
	})
	return nil
}

func (p *recordingUserEventPublisher) PublishUserControl(_ context.Context, event store.UserNodeEvent) error {
	p.mu.Lock()
	defer p.mu.Unlock()

	copy := event
	copy.Frame = append([]byte(nil), event.Frame...)
	p.controls = append(p.controls, copy)
	return nil
}

func (s configurableMeetingStore) CreateMeeting(title, password, hostUserID string, maxParticipants int) (*store.Meeting, *protocol.Participant, error) {
	if s.createMeeting == nil {
		return nil, nil, errors.New("unexpected create call")
	}
	return s.createMeeting(title, password, hostUserID, maxParticipants)
}

func (s configurableMeetingStore) JoinMeeting(meetingID, password, userID string) (*store.Meeting, []*protocol.Participant, *protocol.Participant, error) {
	if s.joinMeeting == nil {
		return nil, nil, nil, errors.New("unexpected join call")
	}
	return s.joinMeeting(meetingID, password, userID)
}

func (s configurableMeetingStore) LeaveMeeting(meetingID, userID string) (bool, *protocol.Participant, int, error) {
	if s.leaveMeeting == nil {
		return false, nil, 0, errors.New("unexpected leave call")
	}
	return s.leaveMeeting(meetingID, userID)
}

func (s configurableMeetingStore) IsMeetingHost(meetingID, userID string) (bool, error) {
	if s.isMeetingHost == nil {
		return false, errors.New("unexpected is host call")
	}
	return s.isMeetingHost(meetingID, userID)
}

func (s configurableMeetingStore) HasParticipant(meetingID, userID string) (bool, error) {
	if s.hasParticipant == nil {
		return false, errors.New("unexpected has participant call")
	}
	return s.hasParticipant(meetingID, userID)
}

func (s configurableMeetingStore) SnapshotMeeting(meetingID string) (*store.Meeting, []*protocol.Participant, error) {
	if s.snapshotMeeting == nil {
		return nil, nil, errors.New("unexpected snapshot call")
	}
	return s.snapshotMeeting(meetingID)
}

type repoBackedMeetingTestState struct {
	mu                sync.Mutex
	nextMeetingID     uint64
	nextParticipantID uint64
	meetingsByNo      map[string]*model.Meeting
	meetingsByID      map[uint64]*model.Meeting
	participants      map[uint64]map[uint64]*model.Participant
}

func newRepoBackedMeetingTestState() *repoBackedMeetingTestState {
	return &repoBackedMeetingTestState{
		nextMeetingID:     1,
		nextParticipantID: 1,
		meetingsByNo:      make(map[string]*model.Meeting),
		meetingsByID:      make(map[uint64]*model.Meeting),
		participants:      make(map[uint64]map[uint64]*model.Participant),
	}
}

type repoBackedMeetingRepo struct {
	state *repoBackedMeetingTestState
	mu    sync.Mutex
	calls []string
}

func (r *repoBackedMeetingRepo) record(call string) {
	r.mu.Lock()
	r.calls = append(r.calls, call)
	r.mu.Unlock()
}

func (r *repoBackedMeetingRepo) called(call string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, current := range r.calls {
		if current == call {
			return true
		}
	}
	return false
}

func (r *repoBackedMeetingRepo) FindByID(ctx context.Context, meetingID uint64) (*model.Meeting, error) {
	_ = ctx
	r.record("find_by_id")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	meeting, ok := r.state.meetingsByID[meetingID]
	if !ok {
		return nil, store.ErrMeetingNotFound
	}
	copy := *meeting
	return &copy, nil
}

func (r *repoBackedMeetingRepo) FindByMeetingNo(ctx context.Context, meetingNo string) (*model.Meeting, error) {
	_ = ctx
	r.record("find_by_meeting_no")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	meeting, ok := r.state.meetingsByNo[meetingNo]
	if !ok {
		return nil, store.ErrMeetingNotFound
	}
	copy := *meeting
	return &copy, nil
}

func (r *repoBackedMeetingRepo) CreateMeeting(ctx context.Context, meeting *model.Meeting) error {
	_ = ctx
	r.record("create_meeting")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	copy := *meeting
	copy.ID = r.state.nextMeetingID
	r.state.nextMeetingID++
	r.state.meetingsByNo[copy.MeetingNo] = &copy
	r.state.meetingsByID[copy.ID] = &copy
	meeting.ID = copy.ID
	return nil
}

func (r *repoBackedMeetingRepo) TransferHost(ctx context.Context, meetingID uint64) (*model.Participant, error) {
	_ = ctx
	r.record("transfer_host")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	bucket := r.state.participants[meetingID]
	var best *model.Participant
	for _, participant := range bucket {
		if participant.LeftAt != nil {
			continue
		}
		if best == nil || participant.JoinedAt.Before(best.JoinedAt) || (participant.JoinedAt.Equal(best.JoinedAt) && participant.ID < best.ID) {
			copy := *participant
			best = &copy
		}
	}
	if best == nil {
		return nil, store.ErrMeetingNotFound
	}

	if meeting, ok := r.state.meetingsByID[meetingID]; ok {
		meeting.HostUserID = best.UserID
	}
	if participant, ok := bucket[best.UserID]; ok {
		participant.Role = 1
	}
	best.Role = 1
	return best, nil
}

func (r *repoBackedMeetingRepo) DeleteMeeting(ctx context.Context, meetingID uint64) error {
	_ = ctx
	r.record("delete_meeting")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	meeting, ok := r.state.meetingsByID[meetingID]
	if !ok {
		return store.ErrMeetingNotFound
	}
	delete(r.state.meetingsByID, meetingID)
	delete(r.state.meetingsByNo, meeting.MeetingNo)
	delete(r.state.participants, meetingID)
	return nil
}

type repoBackedParticipantRepo struct {
	state *repoBackedMeetingTestState
	mu    sync.Mutex
	calls []string
}

func (r *repoBackedParticipantRepo) record(call string) {
	r.mu.Lock()
	r.calls = append(r.calls, call)
	r.mu.Unlock()
}

func (r *repoBackedParticipantRepo) called(call string) bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	for _, current := range r.calls {
		if current == call {
			return true
		}
	}
	return false
}

func (r *repoBackedParticipantRepo) AddParticipant(ctx context.Context, participant *model.Participant) error {
	_ = ctx
	r.record("add_participant")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	bucket, ok := r.state.participants[participant.MeetingID]
	if !ok {
		bucket = make(map[uint64]*model.Participant)
		r.state.participants[participant.MeetingID] = bucket
	}
	copy := *participant
	copy.ID = r.state.nextParticipantID
	r.state.nextParticipantID++
	if copy.JoinedAt.IsZero() {
		copy.JoinedAt = time.Now().UTC()
	}
	copy.LeftAt = nil
	bucket[copy.UserID] = &copy
	participant.ID = copy.ID
	participant.JoinedAt = copy.JoinedAt
	return nil
}

func (r *repoBackedParticipantRepo) MarkParticipantLeft(ctx context.Context, meetingID, userID uint64) error {
	_ = ctx
	r.record("mark_left")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	bucket, ok := r.state.participants[meetingID]
	if !ok {
		return store.ErrMeetingNotFound
	}
	participant, ok := bucket[userID]
	if !ok || participant.LeftAt != nil {
		return store.ErrMeetingNotFound
	}
	leftAt := time.Now().UTC()
	participant.LeftAt = &leftAt
	return nil
}

func (r *repoBackedParticipantRepo) ListActiveParticipants(ctx context.Context, meetingID uint64) ([]model.Participant, error) {
	_ = ctx
	r.record("list_active")
	r.state.mu.Lock()
	defer r.state.mu.Unlock()

	bucket := r.state.participants[meetingID]
	out := make([]model.Participant, 0, len(bucket))
	for _, participant := range bucket {
		if participant.LeftAt != nil {
			continue
		}
		out = append(out, *participant)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].JoinedAt.Equal(out[j].JoinedAt) {
			return out[i].ID < out[j].ID
		}
		return out[i].JoinedAt.Before(out[j].JoinedAt)
	})
	return out, nil
}

func newRepoBackedMeetingStore(memStore *store.MemoryStore) (store.MeetingLifecycleStore, *repoBackedMeetingRepo, *repoBackedParticipantRepo) {
	state := newRepoBackedMeetingTestState()
	meetingRepo := &repoBackedMeetingRepo{state: state}
	participantRepo := &repoBackedParticipantRepo{state: state}
	lifecycle := store.NewMeetingLifecycleStoreWithParticipantRepo(memStore, meetingRepo, participantRepo)
	return lifecycle, meetingRepo, participantRepo
}

func (s failingLeaveMeetingStore) CreateMeeting(title, password, hostUserID string, maxParticipants int) (*store.Meeting, *protocol.Participant, error) {
	_ = title
	_ = password
	_ = hostUserID
	_ = maxParticipants
	return nil, nil, errors.New("unexpected create call")
}

func (s failingLeaveMeetingStore) JoinMeeting(meetingID, password, userID string) (*store.Meeting, []*protocol.Participant, *protocol.Participant, error) {
	_ = meetingID
	_ = password
	_ = userID
	return nil, nil, nil, errors.New("unexpected join call")
}

func (s failingLeaveMeetingStore) LeaveMeeting(meetingID, userID string) (bool, *protocol.Participant, int, error) {
	_ = meetingID
	_ = userID
	return false, nil, 0, s.err
}

func (s failingLeaveMeetingStore) IsMeetingHost(meetingID, userID string) (bool, error) {
	_ = meetingID
	_ = userID
	return false, errors.New("unexpected is host call")
}

func (s failingLeaveMeetingStore) HasParticipant(meetingID, userID string) (bool, error) {
	_ = meetingID
	_ = userID
	return false, errors.New("unexpected has participant call")
}

func (s failingLeaveMeetingStore) SnapshotMeeting(meetingID string) (*store.Meeting, []*protocol.Participant, error) {
	_ = meetingID
	return nil, nil, errors.New("unexpected snapshot call")
}

func TestAuthHandlerResumeTokenRestoreWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, nil, nil)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 101, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", ResumeToken: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected resume token success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId != "u1001" || rsp.Token == "" {
		t.Fatalf("expected restored session for u1001, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
}

func TestAuthHandlerResumeTokenUsernameMismatchFails(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, nil, nil)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 102, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "alice", ResumeToken: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if rsp.Success {
		t.Fatalf("expected resume token mismatch failure, got success: %+v", rsp)
	}
	if rsp.Error == nil || rsp.Error.Code != protocol.ErrAuthFailed {
		t.Fatalf("expected auth failed error, got %+v", rsp.Error)
	}
}

func TestAuthHandlerLegacyTokenRestoreWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, nil, nil)

	token, err := tokenManager.Generate("u1001")
	if err != nil {
		t.Fatalf("generate token failed: %v", err)
	}

	sess, client, cleanup := newRunningSession(t, 103, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", PasswordHash: token}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected legacy token restore success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId != "u1001" || rsp.Token == "" {
		t.Fatalf("expected restored session for u1001, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
}

func TestAuthHandlerLoginWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, nil, nil)

	sess, client, cleanup := newRunningSession(t, 1, cfg)
	defer cleanup()
	sessions.Add(sess)

	req := &protocol.AuthLoginReqBody{Username: "demo", PasswordHash: "demo"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}

	var rsp protocol.AuthLoginRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}

	if !rsp.Success {
		t.Fatalf("expected login success, got failure: %+v", rsp.Error)
	}
	if rsp.UserId == "" || rsp.Token == "" {
		t.Fatalf("expected non-empty user/token, got user=%q token=%q", rsp.UserId, rsp.Token)
	}
	if sess.State() != server.StateAuthenticated {
		t.Fatalf("expected authenticated state, got %v", sess.State())
	}
}

func TestAuthHandlerLoginAndHeartbeatSyncSessionPresence(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-a"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sessionStore := newRecordingSessionStore()
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, sessionStore, nil)

	sess, client, cleanup := newRunningSession(t, 11, cfg)
	defer cleanup()
	sessions.Add(sess)

	payload, err := proto.Marshal(&protocol.AuthLoginReqBody{Username: "demo", PasswordHash: "demo"})
	if err != nil {
		t.Fatalf("marshal login request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, _ := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected login response type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}
	if len(sessionStore.upserts) != 1 {
		t.Fatalf("expected one session upsert after login, got %d", len(sessionStore.upserts))
	}
	if got := sessionStore.upserts[0]; got.UserID != "u1001" || got.NodeID != "sig-node-a" || got.SessionID != sess.ID || got.MeetingID != "" || got.Status != int32(server.StateAuthenticated) {
		t.Fatalf("unexpected login session presence: %+v", got)
	}

	h.HandleHeartbeat(sess, nil)

	msgType, _ = readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthHeartbeatRsp {
		t.Fatalf("unexpected heartbeat response type: got %v want %v", msgType, protocol.AuthHeartbeatRsp)
	}
	if len(sessionStore.upserts) != 2 {
		t.Fatalf("expected second session upsert after heartbeat, got %d", len(sessionStore.upserts))
	}
	if got := sessionStore.upserts[1]; got.UserID != "u1001" || got.Status != int32(server.StateAuthenticated) {
		t.Fatalf("unexpected heartbeat session presence: %+v", got)
	}
}

func TestAuthHandlerLoginPublishesRemoteKickForExistingPresence(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-a"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sessionStore := newRecordingSessionStore()
	sessionStore.entries["u1001"] = store.SessionPresence{
		UserID:    "u1001",
		NodeID:    "sig-node-b",
		SessionID: 501,
		Status:    int32(server.StateAuthenticated),
	}
	nodeBus := &recordingUserEventPublisher{}
	h := NewAuthHandler(cfg, sessions, memStore, tokenManager, limiter, sessionStore, nodeBus)

	sess, client, cleanup := newRunningSession(t, 21, cfg)
	defer cleanup()
	sessions.Add(sess)

	payload, err := proto.Marshal(&protocol.AuthLoginReqBody{Username: "demo", PasswordHash: "demo"})
	if err != nil {
		t.Fatalf("marshal login request failed: %v", err)
	}

	h.HandleLogin(sess, payload)

	msgType, _ := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthLoginRsp {
		t.Fatalf("unexpected login response type: got %v want %v", msgType, protocol.AuthLoginRsp)
	}
	if len(nodeBus.controls) != 1 {
		t.Fatalf("expected one remote auth kick control, got %d", len(nodeBus.controls))
	}
	control := nodeBus.controls[0]
	if control.TargetNodeID != "sig-node-b" || control.TargetUserID != "u1001" || control.TargetSessionID != 501 || !control.Close {
		t.Fatalf("unexpected remote auth kick control: %+v", control)
	}
	if msgType, payload := decodeFrameBytes(t, control.Frame, cfg.MaxPayloadBytes); msgType != protocol.AuthKickNotify {
		t.Fatalf("unexpected remote auth kick frame type: got %v want %v", msgType, protocol.AuthKickNotify)
	} else {
		var notify protocol.AuthKickNotifyBody
		if err := proto.Unmarshal(payload, &notify); err != nil {
			t.Fatalf("unmarshal auth kick notify failed: %v", err)
		}
		if notify.Reason != "账号在其他设备登录" {
			t.Fatalf("unexpected auth kick reason: %+v", notify)
		}
	}
}

func TestMeetingHandlerCreateWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	sess, client, cleanup := newRunningSession(t, 2, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	req := &protocol.MeetCreateReqBody{Title: "proto-room", MaxParticipants: 3}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType1, rspPayload1 := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected first msg type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	var createRsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(rspPayload1, &createRsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if !createRsp.Success || createRsp.MeetingId == "" {
		t.Fatalf("expected create success with meeting id, got %+v", createRsp)
	}

	msgType2, rspPayload2 := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected second msg type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(rspPayload2, &joinRsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if !joinRsp.Success || len(joinRsp.Participants) != 1 {
		t.Fatalf("expected auto-join success with 1 participant, got %+v", joinRsp)
	}
	if joinRsp.Participants[0].UserId != "u1001" {
		t.Fatalf("unexpected host user id: %q", joinRsp.Participants[0].UserId)
	}
	if sess.State() != server.StateInMeeting {
		t.Fatalf("expected in-meeting state, got %v", sess.State())
	}
}

func TestMeetingHandlerCreateAndLeaveSyncSessionPresence(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-a"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sessionStore := newRecordingSessionStore()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, sessionStore, nil, nil, nil)

	sess, client, cleanup := newRunningSession(t, 12, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "presence-room", MaxParticipants: 3})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, createPayload)

	_, _ = readFrame(t, client, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, client, cfg.MaxPayloadBytes)

	if len(sessionStore.upserts) == 0 {
		t.Fatal("expected create to sync session presence")
	}
	createdMeetingID := sess.MeetingID()
	if got := sessionStore.upserts[len(sessionStore.upserts)-1]; got.MeetingID != createdMeetingID || got.Status != int32(server.StateInMeeting) {
		t.Fatalf("unexpected create session presence: %+v", got)
	}

	h.HandleLeave(sess, nil)

	msgType, _ := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected leave response type: got %v want %v", msgType, protocol.MeetLeaveRsp)
	}
	if got := sessionStore.upserts[len(sessionStore.upserts)-1]; got.MeetingID != "" || got.Status != int32(server.StateAuthenticated) {
		t.Fatalf("unexpected leave session presence: %+v", got)
	}
}

func TestMeetingHandlerKickPublishesRemoteControlForRemoteParticipant(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-a"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sessionStore := newRecordingSessionStore()
	sessionStore.entries["u1002"] = store.SessionPresence{
		UserID:    "u1002",
		NodeID:    "sig-node-b",
		SessionID: 602,
		MeetingID: "m-remote-kick",
		Status:    int32(server.StateInMeeting),
	}
	nodeBus := &recordingUserEventPublisher{}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, sessionStore, nodeBus, nil, nil)

	meeting, _, err := memStore.CreateMeeting("remote-kick", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}
	sessionStore.entries["u1002"] = store.SessionPresence{
		UserID:    "u1002",
		NodeID:    "sig-node-b",
		SessionID: 602,
		MeetingID: meeting.ID,
		Status:    int32(server.StateInMeeting),
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 22, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal kick request failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	notifyType, notifyPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if notifyType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected host notify type: got %v want %v", notifyType, protocol.MeetParticipantLeave)
	}
	var hostNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(notifyPayload, &hostNotify); err != nil {
		t.Fatalf("unmarshal host leave notify failed: %v", err)
	}
	if hostNotify.UserId != "u1002" || hostNotify.Reason != "被主持人移出会议" {
		t.Fatalf("unexpected host leave notify: %+v", hostNotify)
	}

	hostStateSync := readMeetStateSync(t, hostConn, cfg.MaxPayloadBytes)
	if hostStateSync.HostId != "u1001" {
		t.Fatalf("unexpected host id in remote kick state sync: %+v", hostStateSync)
	}
	if got := participantIDs(hostStateSync.Participants); len(got) != 1 || got[0] != "u1001" {
		t.Fatalf("unexpected remote kick state sync participants: %+v", got)
	}

	msgType, rspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetKickRsp {
		t.Fatalf("unexpected kick response type: got %v want %v", msgType, protocol.MeetKickRsp)
	}
	var rsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal kick response failed: %v", err)
	}
	if !rsp.Success {
		t.Fatalf("expected remote kick success")
	}
	if len(nodeBus.controls) != 1 {
		t.Fatalf("expected one remote kick control, got %d", len(nodeBus.controls))
	}
	control := nodeBus.controls[0]
	if control.TargetNodeID != "sig-node-b" || control.TargetUserID != "u1002" || control.TargetSessionID != 602 || !control.ResetMeeting || control.State == nil || *control.State != int32(server.StateAuthenticated) {
		t.Fatalf("unexpected remote kick control: %+v", control)
	}
	if msgType, payload := decodeFrameBytes(t, control.Frame, cfg.MaxPayloadBytes); msgType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected remote kick frame type: got %v want %v", msgType, protocol.MeetParticipantLeave)
	} else {
		var notify protocol.MeetParticipantLeaveNotifyBody
		if err := proto.Unmarshal(payload, &notify); err != nil {
			t.Fatalf("unmarshal remote kick notify failed: %v", err)
		}
		if notify.UserId != "u1002" || notify.Reason != "被主持人移出会议" {
			t.Fatalf("unexpected remote kick notify: %+v", notify)
		}
	}
}

func TestMeetingHandlerRepoBackedCreateJoinWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	meetingStore, meetingRepo, participantRepo := newRepoBackedMeetingStore(memStore)
	h := NewMeetingHandler(cfg, sessions, meetingStore, roomState, nil, nil, nil, nil)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 201, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "repo-room", MaxParticipants: 4})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(hostSess, createPayload)

	createType, createRspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if createType != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create response type: got %v want %v", createType, protocol.MeetCreateRsp)
	}
	var createRsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(createRspPayload, &createRsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if !createRsp.Success || createRsp.MeetingId == "" {
		t.Fatalf("expected create success, got %+v", createRsp)
	}

	joinType, joinRspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected auto-join type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	var hostJoinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinRspPayload, &hostJoinRsp); err != nil {
		t.Fatalf("unmarshal auto-join response failed: %v", err)
	}
	if !hostJoinRsp.Success || len(hostJoinRsp.Participants) != 1 || hostJoinRsp.Participants[0].UserId != "u1001" {
		t.Fatalf("unexpected host auto-join response: %+v", hostJoinRsp)
	}
	if hostSess.MeetingID() != createRsp.MeetingId || hostSess.State() != server.StateInMeeting {
		t.Fatalf("expected host session in meeting, state=%v meeting=%q", hostSess.State(), hostSess.MeetingID())
	}

	peerSess, peerConn, peerCleanup := newRunningSession(t, 202, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: createRsp.MeetingId})
	if err != nil {
		t.Fatalf("marshal join request failed: %v", err)
	}

	h.HandleJoin(peerSess, joinPayload)

	peerJoinType, peerJoinPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerJoinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected peer join response type: got %v want %v", peerJoinType, protocol.MeetJoinRsp)
	}
	var peerJoinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(peerJoinPayload, &peerJoinRsp); err != nil {
		t.Fatalf("unmarshal peer join response failed: %v", err)
	}
	if !peerJoinRsp.Success || len(peerJoinRsp.Participants) != 2 {
		t.Fatalf("expected repo-backed join success with 2 participants, got %+v", peerJoinRsp)
	}

	notifyType, notifyPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if notifyType != protocol.MeetParticipantJoin {
		t.Fatalf("unexpected host join notify type: got %v want %v", notifyType, protocol.MeetParticipantJoin)
	}
	var notify protocol.MeetParticipantJoinNotifyBody
	if err := proto.Unmarshal(notifyPayload, &notify); err != nil {
		t.Fatalf("unmarshal join notify failed: %v", err)
	}
	if notify.Participant == nil || notify.Participant.UserId != "u1002" {
		t.Fatalf("unexpected join notify payload: %+v", notify)
	}

	hostStateSync := readMeetStateSync(t, hostConn, cfg.MaxPayloadBytes)
	if hostStateSync.HostId != "u1001" {
		t.Fatalf("unexpected repo-backed host id in state sync: %+v", hostStateSync)
	}
	if got := participantIDs(hostStateSync.Participants); len(got) != 2 || got[0] != "u1001" || got[1] != "u1002" {
		t.Fatalf("unexpected repo-backed state sync participants: %+v", got)
	}

	peerStateSync := readMeetStateSync(t, peerConn, cfg.MaxPayloadBytes)
	if peerStateSync.HostId != "u1001" {
		t.Fatalf("unexpected peer host id in state sync: %+v", peerStateSync)
	}
	if got := participantIDs(peerStateSync.Participants); len(got) != 2 || got[0] != "u1001" || got[1] != "u1002" {
		t.Fatalf("unexpected peer state sync participants: %+v", got)
	}

	if !meetingRepo.called("create_meeting") || !meetingRepo.called("find_by_meeting_no") {
		t.Fatalf("expected meeting repo calls, got %+v", meetingRepo.calls)
	}
	if !participantRepo.called("add_participant") || !participantRepo.called("list_active") {
		t.Fatalf("expected participant repo calls, got %+v", participantRepo.calls)
	}
	if _, err := memStore.Participants(createRsp.MeetingId); err != store.ErrMeetingNotFound {
		t.Fatalf("expected meeting to stay out of memory store, got %v", err)
	}
}

func TestMeetingHandlerCreateIncludesIceServersAndSfuAddress(t *testing.T) {
	cfg := config.Load()
	cfg.DefaultSFUAddress = "sfu.example.com:10000"
	cfg.DefaultSFUNodeID = "sfu-node-01"
	cfg.TURNSecret = "turn-secret"
	cfg.TURNCredTTL = 24 * time.Hour
	cfg.TURNServers = []string{
		"stun:stun.example.com:3478",
		"turn:turn.example.com:3478?transport=udp",
	}

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	sess, client, cleanup := newRunningSession(t, 205, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "ice-room"})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType1, _ := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected first response type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}

	msgType2, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join response type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}
	var rsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if rsp.SfuAddress != "sfu.example.com:10000" {
		t.Fatalf("unexpected sfu address: %q", rsp.SfuAddress)
	}
	if len(rsp.IceServers) != 2 {
		t.Fatalf("expected 2 ice servers, got %+v", rsp.IceServers)
	}
	if rsp.IceServers[0].Urls != "stun:stun.example.com:3478" || rsp.IceServers[0].Username != "" || rsp.IceServers[0].Credential != "" {
		t.Fatalf("unexpected stun server payload: %+v", rsp.IceServers[0])
	}
	if rsp.IceServers[1].Urls != "turn:turn.example.com:3478?transport=udp" || rsp.IceServers[1].Username == "" || rsp.IceServers[1].Credential == "" {
		t.Fatalf("unexpected turn server payload: %+v", rsp.IceServers[1])
	}
}

func TestMeetingHandlerCreateUsesPersistedRoomRouteFromSFU(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.DefaultSFUAddress = "fallback.example.com:10000"
	cfg.DefaultSFUNodeID = "sfu-node-01"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sfuClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{
			Success:    true,
			SfuAddress: "assigned.example.com:10000",
		},
	}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, sfuClient, nil)

	sess, client, cleanup := newRunningSession(t, 206, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "route-room"})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType1, createPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create response type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	var createRsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(createPayload, &createRsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if !createRsp.Success || createRsp.MeetingId == "" {
		t.Fatalf("expected create success, got %+v", createRsp)
	}

	msgType2, joinPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join response type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if joinRsp.SfuAddress != "assigned.example.com:10000" {
		t.Fatalf("expected join to use persisted room route, got %q", joinRsp.SfuAddress)
	}
	if len(sfuClient.createRoomRequests) != 1 || sfuClient.createRoomRequests[0].MeetingId != createRsp.MeetingId {
		t.Fatalf("unexpected create room requests: %+v", sfuClient.createRoomRequests)
	}

	meta, err := roomState.RoomMetadata(context.Background(), createRsp.MeetingId)
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUAddress != "assigned.example.com:10000" || meta.SFUNodeID != "sfu-node-01" {
		t.Fatalf("unexpected room metadata: %+v", meta)
	}
}

func TestMeetingHandlerCreateSFUFailureRollsBackMeeting(t *testing.T) {
	cfg := config.Load()

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sfuClient := &stubSFUClient{createRoomErr: errors.New("sfu unavailable")}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, sfuClient, nil)

	sess, client, cleanup := newRunningSession(t, 207, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "broken-room"})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create response type: got %v want %v", msgType, protocol.MeetCreateRsp)
	}
	var rsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if rsp.Success || rsp.Error == nil || rsp.Error.Code != protocol.ErrInternal {
		t.Fatalf("expected internal create failure, got %+v", rsp)
	}
	if len(sfuClient.createRoomRequests) != 1 {
		t.Fatalf("expected one create room attempt, got %+v", sfuClient.createRoomRequests)
	}
	if sess.State() != server.StateAuthenticated || sess.MeetingID() != "" {
		t.Fatalf("expected session to stay authenticated outside meeting, state=%v meeting=%q", sess.State(), sess.MeetingID())
	}
	if _, err := memStore.Participants(sfuClient.createRoomRequests[0].MeetingId); !errors.Is(err, store.ErrMeetingNotFound) {
		t.Fatalf("expected rolled-back meeting to disappear, err=%v", err)
	}
	assertNoFrame(t, client, "create failure auto-join")
}

func TestMeetingHandlerCreateAllocatesConfiguredSFUNodesRoundRobin(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.DefaultSFUNodeID = "unused-default"
	cfg.DefaultSFUAddress = "unused-default:10000"
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000"},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000"},
	}

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	defaultClient := &stubSFUClient{}
	nodeAClient := &stubSFUClient{createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "media-a:10000"}}
	nodeBClient := &stubSFUClient{createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "media-b:10000"}}
	sfuClient := signalingSfu.NewRoutedClientWithClients(defaultClient, roomState, map[string]signalingSfu.Client{
		"media-a:10000": nodeAClient,
		"media-b:10000": nodeBClient,
	})
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, sfuClient, nil)

	createMeeting := func(sessionID uint64, userID, title string) (string, *protocol.MeetJoinRspBody) {
		sess, client, cleanup := newRunningSession(t, sessionID, cfg)
		defer cleanup()
		sessions.Add(sess)
		sessions.BindUser(sess, userID)
		sess.SetState(server.StateAuthenticated)

		payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: title})
		if err != nil {
			t.Fatalf("marshal create request failed: %v", err)
		}

		h.HandleCreate(sess, payload)

		msgType1, createPayload := readFrame(t, client, cfg.MaxPayloadBytes)
		if msgType1 != protocol.MeetCreateRsp {
			t.Fatalf("unexpected create response type: got %v want %v", msgType1, protocol.MeetCreateRsp)
		}
		var createRsp protocol.MeetCreateRspBody
		if err := proto.Unmarshal(createPayload, &createRsp); err != nil {
			t.Fatalf("unmarshal create response failed: %v", err)
		}
		if !createRsp.Success || createRsp.MeetingId == "" {
			t.Fatalf("expected create success, got %+v", createRsp)
		}

		msgType2, joinPayload := readFrame(t, client, cfg.MaxPayloadBytes)
		if msgType2 != protocol.MeetJoinRsp {
			t.Fatalf("unexpected join response type: got %v want %v", msgType2, protocol.MeetJoinRsp)
		}
		var joinRsp protocol.MeetJoinRspBody
		if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
			t.Fatalf("unmarshal join response failed: %v", err)
		}
		return createRsp.MeetingId, &joinRsp
	}

	meetingA, joinA := createMeeting(208, "u1001", "rr-room-a")
	meetingB, joinB := createMeeting(209, "u1002", "rr-room-b")

	if joinA.SfuAddress != "media-a:10000" || joinB.SfuAddress != "media-b:10000" {
		t.Fatalf("unexpected round-robin routes: first=%q second=%q", joinA.SfuAddress, joinB.SfuAddress)
	}
	if len(defaultClient.createRoomRequests) != 0 {
		t.Fatalf("expected default client not to handle configured-node creates, got %+v", defaultClient.createRoomRequests)
	}
	if len(nodeAClient.createRoomRequests) != 1 || nodeAClient.createRoomRequests[0].MeetingId != meetingA {
		t.Fatalf("unexpected node A create requests: %+v", nodeAClient.createRoomRequests)
	}
	if len(nodeBClient.createRoomRequests) != 1 || nodeBClient.createRoomRequests[0].MeetingId != meetingB {
		t.Fatalf("unexpected node B create requests: %+v", nodeBClient.createRoomRequests)
	}

	metaA, err := roomState.RoomMetadata(context.Background(), meetingA)
	if err != nil {
		t.Fatalf("load meeting A metadata failed: %v", err)
	}
	metaB, err := roomState.RoomMetadata(context.Background(), meetingB)
	if err != nil {
		t.Fatalf("load meeting B metadata failed: %v", err)
	}
	if metaA == nil || metaA.SFUNodeID != "sfu-a" || metaA.SFUAddress != "media-a:10000" {
		t.Fatalf("unexpected meeting A metadata: %+v", metaA)
	}
	if metaB == nil || metaB.SFUNodeID != "sfu-b" || metaB.SFUAddress != "media-b:10000" {
		t.Fatalf("unexpected meeting B metadata: %+v", metaB)
	}
}

func TestMeetingHandlerCreateFailsOverAndQuarantinesFailedSFUNode(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000"},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000"},
	}

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	defaultClient := &stubSFUClient{}
	nodeAClient := &stubSFUClient{createRoomErr: errors.New("dial failed")}
	nodeBClient := &stubSFUClient{createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "media-b:10000"}}
	sfuClient := signalingSfu.NewRoutedClientWithClients(defaultClient, roomState, map[string]signalingSfu.Client{
		"media-a:10000": nodeAClient,
		"media-b:10000": nodeBClient,
	})
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, sfuClient, nil)

	runCreate := func(sessionID uint64, userID, title string) (string, *protocol.MeetJoinRspBody) {
		sess, client, cleanup := newRunningSession(t, sessionID, cfg)
		defer cleanup()
		sessions.Add(sess)
		sessions.BindUser(sess, userID)
		sess.SetState(server.StateAuthenticated)

		payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: title})
		if err != nil {
			t.Fatalf("marshal create request failed: %v", err)
		}

		h.HandleCreate(sess, payload)

		msgType1, createPayload := readFrame(t, client, cfg.MaxPayloadBytes)
		if msgType1 != protocol.MeetCreateRsp {
			t.Fatalf("unexpected create response type: got %v want %v", msgType1, protocol.MeetCreateRsp)
		}
		var createRsp protocol.MeetCreateRspBody
		if err := proto.Unmarshal(createPayload, &createRsp); err != nil {
			t.Fatalf("unmarshal create response failed: %v", err)
		}
		if !createRsp.Success || createRsp.MeetingId == "" {
			t.Fatalf("expected create success, got %+v", createRsp)
		}

		msgType2, joinPayload := readFrame(t, client, cfg.MaxPayloadBytes)
		if msgType2 != protocol.MeetJoinRsp {
			t.Fatalf("unexpected join response type: got %v want %v", msgType2, protocol.MeetJoinRsp)
		}
		var joinRsp protocol.MeetJoinRspBody
		if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
			t.Fatalf("unmarshal join response failed: %v", err)
		}
		return createRsp.MeetingId, &joinRsp
	}

	meetingOne, joinOne := runCreate(210, "u1001", "failover-room-one")
	meetingTwo, joinTwo := runCreate(211, "u1002", "failover-room-two")

	if joinOne.SfuAddress != "media-b:10000" || joinTwo.SfuAddress != "media-b:10000" {
		t.Fatalf("expected both creates to land on healthy node B, first=%q second=%q", joinOne.SfuAddress, joinTwo.SfuAddress)
	}
	if len(nodeAClient.createRoomRequests) != 1 {
		t.Fatalf("expected failed node A to be tried only once before quarantine, got %+v", nodeAClient.createRoomRequests)
	}
	if len(nodeBClient.createRoomRequests) != 2 {
		t.Fatalf("expected healthy node B to serve both creates, got %+v", nodeBClient.createRoomRequests)
	}

	metaOne, err := roomState.RoomMetadata(context.Background(), meetingOne)
	if err != nil {
		t.Fatalf("load first meeting metadata failed: %v", err)
	}
	metaTwo, err := roomState.RoomMetadata(context.Background(), meetingTwo)
	if err != nil {
		t.Fatalf("load second meeting metadata failed: %v", err)
	}
	if metaOne == nil || metaOne.SFUNodeID != "sfu-b" || metaTwo == nil || metaTwo.SFUNodeID != "sfu-b" {
		t.Fatalf("unexpected failover metadata: first=%+v second=%+v", metaOne, metaTwo)
	}
}

func TestMeetingHandlerCreateUsesActivelyRegisteredSFUNode(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.DefaultSFUNodeID = "unused-default"
	cfg.DefaultSFUAddress = "unused-default:10000"
	cfg.SFUNodes = nil

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	if err := roomState.ReportSFUNode(context.Background(), config.SFUNode{
		NodeID:       "sfu-live-01",
		MediaAddress: "media-live:10000",
		RPCAddress:   "rpc-live:9000",
		MaxMeetings:  12,
	}, store.SFUNodeStatus{
		SFUAddress:     "media-live:10000",
		MediaPort:      10000,
		RoomCount:      0,
		PublisherCount: 0,
	}, time.Minute); err != nil {
		t.Fatalf("report active node failed: %v", err)
	}

	defaultClient := &stubSFUClient{}
	liveClient := &stubSFUClient{createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "media-live:10000"}}
	sfuClient := signalingSfu.NewRoutedClientWithClients(defaultClient, roomState, map[string]signalingSfu.Client{
		"media-live:10000": liveClient,
	})
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, sfuClient, nil)

	sess, client, cleanup := newRunningSession(t, 212, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "active-registration-room"})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType1, createPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create response type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	var createRsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(createPayload, &createRsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if !createRsp.Success || createRsp.MeetingId == "" {
		t.Fatalf("expected create success, got %+v", createRsp)
	}

	msgType2, joinPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join response type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if joinRsp.SfuAddress != "media-live:10000" {
		t.Fatalf("unexpected join route for active node: %+v", joinRsp)
	}
	if len(defaultClient.createRoomRequests) != 0 {
		t.Fatalf("expected default client to stay unused, got %+v", defaultClient.createRoomRequests)
	}
	if len(liveClient.createRoomRequests) != 1 || liveClient.createRoomRequests[0].MeetingId != createRsp.MeetingId {
		t.Fatalf("unexpected active node create requests: %+v", liveClient.createRoomRequests)
	}
}

func TestMeetingHandlerCreateStoreFailureReturnsInternalError(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, configurableMeetingStore{
		createMeeting: func(title, password, hostUserID string, maxParticipants int) (*store.Meeting, *protocol.Participant, error) {
			_ = title
			_ = password
			_ = hostUserID
			_ = maxParticipants
			return nil, nil, errors.New("db unavailable")
		},
	}, roomState, nil, nil, nil, nil)

	sess, client, cleanup := newRunningSession(t, 203, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "broken-room"})
	if err != nil {
		t.Fatalf("marshal create request failed: %v", err)
	}

	h.HandleCreate(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create response type: got %v want %v", msgType, protocol.MeetCreateRsp)
	}
	var rsp protocol.MeetCreateRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal create response failed: %v", err)
	}
	if rsp.Success || rsp.Error == nil || rsp.Error.Code != protocol.ErrInternal {
		t.Fatalf("expected internal error for create failure, got %+v", rsp)
	}
}

func TestMeetingHandlerJoinBroadcastParticipantNotify(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, _, err := memStore.CreateMeeting("join-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 4, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 5, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	req := &protocol.MeetJoinReqBody{MeetingId: meeting.ID}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}

	h.HandleJoin(peerSess, payload)

	joinType, joinPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected peer join rsp type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinPayload, &joinRsp); err != nil {
		t.Fatalf("unmarshal join rsp failed: %v", err)
	}
	if !joinRsp.Success || len(joinRsp.Participants) != 2 {
		t.Fatalf("expected join success with 2 participants, got %+v", joinRsp)
	}

	notifyType, notifyPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if notifyType != protocol.MeetParticipantJoin {
		t.Fatalf("unexpected host notify type: got %v want %v", notifyType, protocol.MeetParticipantJoin)
	}
	var notify protocol.MeetParticipantJoinNotifyBody
	if err := proto.Unmarshal(notifyPayload, &notify); err != nil {
		t.Fatalf("unmarshal join notify failed: %v", err)
	}
	if notify.Participant == nil || notify.Participant.UserId != "u1002" {
		t.Fatalf("unexpected join notify payload: %+v", notify)
	}

	hostStateSync := readMeetStateSync(t, hostConn, cfg.MaxPayloadBytes)
	if hostStateSync.HostId != "u1001" {
		t.Fatalf("unexpected host id in state sync: %+v", hostStateSync)
	}
	if got := participantIDs(hostStateSync.Participants); len(got) != 2 || got[0] != "u1001" || got[1] != "u1002" {
		t.Fatalf("unexpected host state sync participants: %+v", got)
	}

	peerStateSync := readMeetStateSync(t, peerConn, cfg.MaxPayloadBytes)
	if peerStateSync.HostId != "u1001" {
		t.Fatalf("unexpected peer host id in state sync: %+v", peerStateSync)
	}
	if got := participantIDs(peerStateSync.Participants); len(got) != 2 || got[0] != "u1001" || got[1] != "u1002" {
		t.Fatalf("unexpected peer state sync participants: %+v", got)
	}
}

func TestMeetingHandlerJoinStoreFailureReturnsInternalError(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, configurableMeetingStore{
		joinMeeting: func(meetingID, password, userID string) (*store.Meeting, []*protocol.Participant, *protocol.Participant, error) {
			_ = meetingID
			_ = password
			_ = userID
			return nil, nil, nil, errors.New("repo timeout")
		},
	}, roomState, nil, nil, nil, nil)

	sess, client, cleanup := newRunningSession(t, 204, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1002")
	sess.SetState(server.StateAuthenticated)

	payload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: "123456"})
	if err != nil {
		t.Fatalf("marshal join request failed: %v", err)
	}

	h.HandleJoin(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join response type: got %v want %v", msgType, protocol.MeetJoinRsp)
	}
	var rsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if rsp.Success || rsp.Error == nil || rsp.Error.Code != protocol.ErrInternal {
		t.Fatalf("expected internal error for join failure, got %+v", rsp)
	}
}

type meetingMirrorCall struct {
	kind      string
	meetingID string
	userID    string
	reason    string
	title     string
}

type recordingMeetingMirror struct {
	mu    sync.Mutex
	calls []meetingMirrorCall
}

func (m *recordingMeetingMirror) append(call meetingMirrorCall) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.calls = append(m.calls, call)
}

func (m *recordingMeetingMirror) snapshot() []meetingMirrorCall {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]meetingMirrorCall, len(m.calls))
	copy(out, m.calls)
	return out
}

func (m *recordingMeetingMirror) MirrorCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error {
	_ = ctx
	call := meetingMirrorCall{kind: "create"}
	if meeting != nil {
		call.meetingID = meeting.ID
		call.title = meeting.Title
	}
	if host != nil {
		call.userID = host.UserId
	}
	m.append(call)
	return nil
}

func (m *recordingMeetingMirror) MirrorJoin(ctx context.Context, meeting *store.Meeting, participant *protocol.Participant) error {
	_ = ctx
	call := meetingMirrorCall{kind: "join"}
	if meeting != nil {
		call.meetingID = meeting.ID
		call.title = meeting.Title
	}
	if participant != nil {
		call.userID = participant.UserId
	}
	m.append(call)
	return nil
}

func (m *recordingMeetingMirror) MirrorLeave(ctx context.Context, meetingID, userID, reason string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "leave", meetingID: meetingID, userID: userID, reason: reason})
	return nil
}

func (m *recordingMeetingMirror) MirrorTransferHost(ctx context.Context, meetingID, newHostUserID string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "transfer", meetingID: meetingID, userID: newHostUserID})
	return nil
}

func (m *recordingMeetingMirror) MirrorDelete(ctx context.Context, meetingID string) error {
	_ = ctx
	m.append(meetingMirrorCall{kind: "delete", meetingID: meetingID})
	return nil
}

func TestMeetingHandlerMirrorCallbacksOnLifecycle(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	recorder := &recordingMeetingMirror{}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, recorder)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 61, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "mirror-room", MaxParticipants: 4})
	if err != nil {
		t.Fatalf("marshal create req failed: %v", err)
	}
	h.HandleCreate(hostSess, createPayload)

	msgType1, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetCreateRsp {
		t.Fatalf("unexpected create rsp type: got %v want %v", msgType1, protocol.MeetCreateRsp)
	}
	msgType2, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType2 != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join rsp type: got %v want %v", msgType2, protocol.MeetJoinRsp)
	}

	calls := recorder.snapshot()
	if len(calls) != 1 || calls[0].kind != "create" {
		t.Fatalf("expected create mirror call, got %+v", calls)
	}
	meetingID := calls[0].meetingID
	if meetingID == "" {
		t.Fatalf("expected create mirror call to include meeting id, got %+v", calls)
	}

	peerSess, peerConn, peerCleanup := newRunningSession(t, 62, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: meetingID})
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}
	h.HandleJoin(peerSess, joinPayload)

	joinType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join rsp type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, peerConn, cfg.MaxPayloadBytes)

	calls = recorder.snapshot()
	if len(calls) != 2 || calls[1].kind != "join" || calls[1].userID != "u1002" || calls[1].meetingID != meetingID {
		t.Fatalf("expected peer join mirror call, got %+v", calls)
	}

	leavePayload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}
	h.HandleLeave(peerSess, leavePayload)

	leaveType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if leaveType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected leave rsp type: got %v want %v", leaveType, protocol.MeetLeaveRsp)
	}
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	calls = recorder.snapshot()
	if len(calls) != 3 || calls[2].kind != "leave" || calls[2].reason != "主动离开" {
		t.Fatalf("expected leave mirror call, got %+v", calls)
	}
}

func TestMeetingHandlerMirrorCallbacksOnKick(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	recorder := &recordingMeetingMirror{}
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, recorder)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 63, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetState(server.StateAuthenticated)

	createPayload, err := proto.Marshal(&protocol.MeetCreateReqBody{Title: "kick-mirror", MaxParticipants: 4})
	if err != nil {
		t.Fatalf("marshal create req failed: %v", err)
	}
	h.HandleCreate(hostSess, createPayload)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)

	calls := recorder.snapshot()
	meetingID := calls[0].meetingID

	peerSess, peerConn, peerCleanup := newRunningSession(t, 64, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: meetingID})
	if err != nil {
		t.Fatalf("marshal join req failed: %v", err)
	}
	h.HandleJoin(peerSess, joinPayload)
	_, _ = readFrame(t, peerConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	_, _ = readFrame(t, peerConn, cfg.MaxPayloadBytes)

	kickPayload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal kick req failed: %v", err)
	}
	h.HandleKick(hostSess, kickPayload)

	kickType, _ := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if kickType == protocol.MeetParticipantLeave {
		_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
		kickType, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	}
	if kickType != protocol.MeetKickRsp {
		t.Fatalf("unexpected kick rsp type: got %v want %v", kickType, protocol.MeetKickRsp)
	}
	leaveType, _ := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if leaveType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected kick leave notify type: got %v want %v", leaveType, protocol.MeetParticipantLeave)
	}

	calls = recorder.snapshot()
	if len(calls) != 3 || calls[2].kind != "leave" || calls[2].reason != "被主持人移出会议" {
		t.Fatalf("expected kick mirror leave call, got %+v", calls)
	}
}
func TestChatHandlerSendWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	h := NewChatHandler(cfg, sessions, memStore)

	sess, client, cleanup := newRunningSession(t, 3, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u1001")
	sess.SetMeetingID("m-test")
	sess.SetState(server.StateInMeeting)

	req := &protocol.ChatSendReqBody{Type: 1, Content: "hello protobuf"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal request failed: %v", err)
	}

	h.HandleSend(sess, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.ChatSendRsp {
		t.Fatalf("unexpected msg type: got %v want %v", msgType, protocol.ChatSendRsp)
	}

	var rsp protocol.ChatSendRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal response failed: %v", err)
	}
	if !rsp.Success || rsp.MessageId == "" {
		t.Fatalf("expected chat send success with message id, got %+v", rsp)
	}
}

func TestMeetingHandlerKickWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, _, err := memStore.CreateMeeting("kick-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 11, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 12, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	req := &protocol.MeetKickReqBody{TargetUserId: "u1002"}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal kick req failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	hostLeaveType, hostLeavePayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostLeaveType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected host leave notify type: got %v want %v", hostLeaveType, protocol.MeetParticipantLeave)
	}
	var hostLeave protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostLeavePayload, &hostLeave); err != nil {
		t.Fatalf("unmarshal host leave notify failed: %v", err)
	}
	if hostLeave.UserId != "u1002" || hostLeave.Reason != "被主持人移出会议" {
		t.Fatalf("unexpected host leave notify payload: %+v", hostLeave)
	}

	hostStateSync := readMeetStateSync(t, hostConn, cfg.MaxPayloadBytes)
	if hostStateSync.HostId != "u1001" {
		t.Fatalf("unexpected host id in kick state sync: %+v", hostStateSync)
	}
	if got := participantIDs(hostStateSync.Participants); len(got) != 1 || got[0] != "u1001" {
		t.Fatalf("unexpected kick state sync participants: %+v", got)
	}

	msgType1, payload1 := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType1 != protocol.MeetKickRsp {
		t.Fatalf("unexpected host msg type: got %v want %v", msgType1, protocol.MeetKickRsp)
	}
	var kickRsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(payload1, &kickRsp); err != nil {
		t.Fatalf("unmarshal kick rsp failed: %v", err)
	}
	if !kickRsp.Success {
		t.Fatalf("expected kick success, got %+v", kickRsp)
	}

	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected target notify type: got %v want %v", targetType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(targetPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal target leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" {
		t.Fatalf("unexpected leave user: %s", leaveNotify.UserId)
	}
	assertNoFrame(t, targetConn, "target state sync after kick")

	if targetSess.State() != server.StateAuthenticated || targetSess.MeetingID() != "" {
		t.Fatalf("expected kicked session reset, state=%v meeting=%q", targetSess.State(), targetSess.MeetingID())
	}
}

func TestMeetingHandlerKickStoreFailureReturnsInternalError(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, configurableMeetingStore{
		isMeetingHost: func(meetingID, userID string) (bool, error) {
			_ = meetingID
			_ = userID
			return false, errors.New("redis unavailable")
		},
	}, roomState, nil, nil, nil, nil)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 13, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID("m-kick")
	hostSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal kick request failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	msgType, rspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetKickRsp {
		t.Fatalf("unexpected kick response type: got %v want %v", msgType, protocol.MeetKickRsp)
	}
	var rsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal kick response failed: %v", err)
	}
	if rsp.Success || rsp.Error == nil || rsp.Error.Code != protocol.ErrInternal {
		t.Fatalf("expected internal error for kick failure, got %+v", rsp)
	}
}

func TestMeetingHandlerRepoBackedKickWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	meetingStore, meetingRepo, participantRepo := newRepoBackedMeetingStore(memStore)
	h := NewMeetingHandler(cfg, sessions, meetingStore, roomState, nil, nil, nil, nil)

	meeting, _, err := meetingStore.CreateMeeting("repo-kick-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create repo-backed meeting failed: %v", err)
	}
	if _, _, _, err = meetingStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join repo-backed meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 14, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 15, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal repo-backed kick request failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType == protocol.MeetParticipantLeave {
		_ = hostPayload
		_, _ = readFrame(t, hostConn, cfg.MaxPayloadBytes)
		hostType, hostPayload = readFrame(t, hostConn, cfg.MaxPayloadBytes)
	}
	if hostType != protocol.MeetKickRsp {
		t.Fatalf("unexpected repo-backed host response type: got %v want %v", hostType, protocol.MeetKickRsp)
	}
	var kickRsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(hostPayload, &kickRsp); err != nil {
		t.Fatalf("unmarshal repo-backed kick response failed: %v", err)
	}
	if !kickRsp.Success {
		t.Fatalf("expected repo-backed kick success, got %+v", kickRsp)
	}

	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected repo-backed target notify type: got %v want %v", targetType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(targetPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal repo-backed target leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "被主持人移出会议" {
		t.Fatalf("unexpected repo-backed leave notify: %+v", leaveNotify)
	}

	if targetSess.State() != server.StateAuthenticated || targetSess.MeetingID() != "" {
		t.Fatalf("expected repo-backed kicked session reset, state=%v meeting=%q", targetSess.State(), targetSess.MeetingID())
	}
	if exists, err := meetingStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected repo-backed kicked participant removed, exists=%v err=%v", exists, err)
	}
	if !meetingRepo.called("find_by_meeting_no") || !participantRepo.called("mark_left") {
		t.Fatalf("expected repo-backed kick path to use repos, meetingCalls=%+v participantCalls=%+v", meetingRepo.calls, participantRepo.calls)
	}
	if _, err := memStore.Participants(meeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected repo-backed kick room to stay out of memory store, got %v", err)
	}
}

func TestMeetingHandlerKickLeaveFailureDoesNotResetTargetSession(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, configurableMeetingStore{
		isMeetingHost: func(meetingID, userID string) (bool, error) {
			_ = meetingID
			_ = userID
			return true, nil
		},
		hasParticipant: func(meetingID, userID string) (bool, error) {
			_ = meetingID
			_ = userID
			return true, nil
		},
		leaveMeeting: func(meetingID, userID string) (bool, *protocol.Participant, int, error) {
			_ = meetingID
			_ = userID
			return false, nil, 0, errors.New("leave transaction failed")
		},
	}, roomState, nil, nil, nil, nil)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 16, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID("m-kick-fail")
	hostSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 17, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-kick-fail")
	targetSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetKickReqBody{TargetUserId: "u1002"})
	if err != nil {
		t.Fatalf("marshal kick request failed: %v", err)
	}

	h.HandleKick(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetKickRsp {
		t.Fatalf("unexpected kick failure response type: got %v want %v", hostType, protocol.MeetKickRsp)
	}
	var rsp protocol.MeetKickRspBody
	if err := proto.Unmarshal(hostPayload, &rsp); err != nil {
		t.Fatalf("unmarshal kick failure response failed: %v", err)
	}
	if rsp.Success || rsp.Error == nil || rsp.Error.Code != protocol.ErrInternal {
		t.Fatalf("expected internal error for leaveMeeting failure, got %+v", rsp)
	}

	if targetSess.State() != server.StateInMeeting || targetSess.MeetingID() != "m-kick-fail" {
		t.Fatalf("expected target session to stay untouched on kick failure, state=%v meeting=%q", targetSess.State(), targetSess.MeetingID())
	}
	assertNoFrame(t, targetConn, "target connection on kick leave failure")
}

func TestMeetingHandlerMuteAllWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, _, err := memStore.CreateMeeting("mute-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 21, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 22, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	req := &protocol.MeetMuteAllReqBody{Mute: true}
	payload, err := proto.Marshal(req)
	if err != nil {
		t.Fatalf("marshal mute req failed: %v", err)
	}

	h.HandleMuteAll(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetMuteAllRsp {
		t.Fatalf("unexpected host mute rsp type: got %v want %v", hostType, protocol.MeetMuteAllRsp)
	}
	var muteRsp protocol.MeetMuteAllRspBody
	if err := proto.Unmarshal(hostPayload, &muteRsp); err != nil {
		t.Fatalf("unmarshal mute rsp failed: %v", err)
	}
	if !muteRsp.Success {
		t.Fatalf("expected mute all success")
	}

	peerType, peerPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType != protocol.MeetMuteAllReq {
		t.Fatalf("unexpected peer notify type: got %v want %v", peerType, protocol.MeetMuteAllReq)
	}
	var muteNotify protocol.MeetMuteAllReqBody
	if err := proto.Unmarshal(peerPayload, &muteNotify); err != nil {
		t.Fatalf("unmarshal mute notify failed: %v", err)
	}
	if !muteNotify.Mute {
		t.Fatalf("expected mute=true notify")
	}
}

func TestMeetingHandlerJoinReplaysPersistedMuteAll(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	client := store.NewRedisClient(cfg)
	if client == nil {
		t.Fatal("expected redis client")
	}
	defer client.Close()

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStoreWithClient(client)
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, hostParticipant, err := memStore.CreateMeeting("mute-replay-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 24, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	mutePayload, err := proto.Marshal(&protocol.MeetMuteAllReqBody{Mute: true})
	if err != nil {
		t.Fatalf("marshal mute-all request failed: %v", err)
	}
	h.HandleMuteAll(hostSess, mutePayload)

	msgType, rspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetMuteAllRsp {
		t.Fatalf("unexpected mute-all response type: got %v want %v", msgType, protocol.MeetMuteAllRsp)
	}
	var muteRsp protocol.MeetMuteAllRspBody
	if err := proto.Unmarshal(rspPayload, &muteRsp); err != nil {
		t.Fatalf("unmarshal mute-all response failed: %v", err)
	}
	if !muteRsp.Success {
		t.Fatalf("expected mute-all success, got %+v", muteRsp)
	}

	peerSess, peerConn, peerCleanup := newRunningSession(t, 25, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetState(server.StateAuthenticated)

	joinPayload, err := proto.Marshal(&protocol.MeetJoinReqBody{MeetingId: meeting.ID})
	if err != nil {
		t.Fatalf("marshal join request failed: %v", err)
	}
	h.HandleJoin(peerSess, joinPayload)

	joinType, joinRspPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if joinType != protocol.MeetJoinRsp {
		t.Fatalf("unexpected join response type: got %v want %v", joinType, protocol.MeetJoinRsp)
	}
	var joinRsp protocol.MeetJoinRspBody
	if err := proto.Unmarshal(joinRspPayload, &joinRsp); err != nil {
		t.Fatalf("unmarshal join response failed: %v", err)
	}
	if !joinRsp.Success {
		t.Fatalf("expected join success, got %+v", joinRsp)
	}

	peerStateSync := readMeetStateSync(t, peerConn, cfg.MaxPayloadBytes)
	if peerStateSync.HostId != "u1001" {
		t.Fatalf("unexpected host id in replay state sync: %+v", peerStateSync)
	}

	peerMuteType, peerMutePayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerMuteType != protocol.MeetMuteAllReq {
		t.Fatalf("unexpected replay mute type: got %v want %v", peerMuteType, protocol.MeetMuteAllReq)
	}
	var peerMute protocol.MeetMuteAllReqBody
	if err := proto.Unmarshal(peerMutePayload, &peerMute); err != nil {
		t.Fatalf("unmarshal replay mute request failed: %v", err)
	}
	if !peerMute.Mute {
		t.Fatalf("expected replay mute=true, got %+v", peerMute)
	}
}

func TestMeetingHandlerMuteAllStoreFailureReturnsFalse(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, configurableMeetingStore{
		isMeetingHost: func(meetingID, userID string) (bool, error) {
			_ = meetingID
			_ = userID
			return false, errors.New("room state unavailable")
		},
	}, roomState, nil, nil, nil, nil)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 23, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID("m-mute")
	hostSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetMuteAllReqBody{Mute: true})
	if err != nil {
		t.Fatalf("marshal mute request failed: %v", err)
	}

	h.HandleMuteAll(hostSess, payload)

	msgType, rspPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetMuteAllRsp {
		t.Fatalf("unexpected mute-all response type: got %v want %v", msgType, protocol.MeetMuteAllRsp)
	}
	var rsp protocol.MeetMuteAllRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal mute-all response failed: %v", err)
	}
	if rsp.Success {
		t.Fatalf("expected mute-all failure on store error")
	}
}

func TestMeetingHandlerLeaveWithProtoPayload(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, _, err := memStore.CreateMeeting("leave-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 31, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 32, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(peerSess, payload)

	peerType, peerPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected peer leave rsp type: got %v want %v", peerType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(peerPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected leave success")
	}

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected host notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "主动离开" {
		t.Fatalf("unexpected leave notify: %+v", leaveNotify)
	}

	hostStateSync := readMeetStateSync(t, hostConn, cfg.MaxPayloadBytes)
	if hostStateSync.HostId != "u1001" {
		t.Fatalf("unexpected host id in leave state sync: %+v", hostStateSync)
	}
	if got := participantIDs(hostStateSync.Participants); len(got) != 1 || got[0] != "u1001" {
		t.Fatalf("unexpected leave state sync participants: %+v", got)
	}
	assertNoFrame(t, peerConn, "leaving peer state sync")

	if peerSess.State() != server.StateAuthenticated || peerSess.MeetingID() != "" {
		t.Fatalf("expected leaving session reset, state=%v meeting=%q", peerSess.State(), peerSess.MeetingID())
	}
	if exists, err := memStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected participant removed, exists=%v err=%v", exists, err)
	}
}

func TestMeetingHandlerLeaveTransfersHostAndCleansEmptyRoom(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, memStore, roomState, nil, nil, nil, nil)

	meeting, _, err := memStore.CreateMeeting("host-leave", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting u1002 failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1003"); err != nil {
		t.Fatalf("join meeting u1003 failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 41, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 42, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	thirdSess, thirdConn, thirdCleanup := newRunningSession(t, 43, cfg)
	defer thirdCleanup()
	sessions.Add(thirdSess)
	sessions.BindUser(thirdSess, "u1003")
	thirdSess.SetMeetingID(meeting.ID)
	thirdSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected host leave rsp type: got %v want %v", hostType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(hostPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal host leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected host leave success")
	}

	peerType1, peerPayload1 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected peer leave notify type: got %v want %v", peerType1, protocol.MeetParticipantLeave)
	}
	var peerLeave protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(peerPayload1, &peerLeave); err != nil {
		t.Fatalf("unmarshal peer leave notify failed: %v", err)
	}
	if peerLeave.UserId != "u1001" {
		t.Fatalf("unexpected leave user: %+v", peerLeave)
	}

	peerType2, peerPayload2 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected peer host-changed type: got %v want %v", peerType2, protocol.MeetHostChanged)
	}
	var hostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(peerPayload2, &hostChanged); err != nil {
		t.Fatalf("unmarshal host changed notify failed: %v", err)
	}
	if hostChanged.NewHostId != "u1002" {
		t.Fatalf("expected u1002 to become host, got %+v", hostChanged)
	}

	thirdType1, _ := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected third leave notify type: got %v want %v", thirdType1, protocol.MeetParticipantLeave)
	}
	thirdType2, thirdPayload2 := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected third host-changed type: got %v want %v", thirdType2, protocol.MeetHostChanged)
	}
	var thirdHostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(thirdPayload2, &thirdHostChanged); err != nil {
		t.Fatalf("unmarshal third host changed notify failed: %v", err)
	}
	if thirdHostChanged.NewHostId != "u1002" {
		t.Fatalf("expected u1002 host change for all peers, got %+v", thirdHostChanged)
	}

	peerStateSync := readMeetStateSync(t, peerConn, cfg.MaxPayloadBytes)
	if peerStateSync.HostId != "u1002" {
		t.Fatalf("unexpected peer host id in leave transfer sync: %+v", peerStateSync)
	}
	if got := participantIDs(peerStateSync.Participants); len(got) != 2 || got[0] != "u1002" || got[1] != "u1003" {
		t.Fatalf("unexpected peer leave transfer participants: %+v", got)
	}

	thirdStateSync := readMeetStateSync(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdStateSync.HostId != "u1002" {
		t.Fatalf("unexpected third host id in leave transfer sync: %+v", thirdStateSync)
	}
	if got := participantIDs(thirdStateSync.Participants); len(got) != 2 || got[0] != "u1002" || got[1] != "u1003" {
		t.Fatalf("unexpected third leave transfer participants: %+v", got)
	}

	if isHost, err := memStore.IsMeetingHost(meeting.ID, "u1002"); err != nil || !isHost {
		t.Fatalf("expected u1002 to be stored as host, isHost=%v err=%v", isHost, err)
	}

	cleanupMeeting, _, err := memStore.CreateMeeting("cleanup-room", "", "u1003", 1)
	if err != nil {
		t.Fatalf("create cleanup meeting failed: %v", err)
	}
	lastSess, lastConn, lastCleanup := newRunningSession(t, 44, cfg)
	defer lastCleanup()
	sessions.Add(lastSess)
	sessions.BindUser(lastSess, "u1003")
	lastSess.SetMeetingID(cleanupMeeting.ID)
	lastSess.SetState(server.StateInMeeting)

	h.HandleLeave(lastSess, payload)

	lastType, lastPayload := readFrame(t, lastConn, cfg.MaxPayloadBytes)
	if lastType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected cleanup leave rsp type: got %v want %v", lastType, protocol.MeetLeaveRsp)
	}
	if err := proto.Unmarshal(lastPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal cleanup leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected cleanup leave success")
	}
	if _, err := memStore.Participants(cleanupMeeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected empty room cleanup, got %v", err)
	}
}

func TestMeetingHandlerRepoBackedLeaveTransfersHostAndCleansEmptyRoom(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	meetingStore, meetingRepo, participantRepo := newRepoBackedMeetingStore(memStore)
	h := NewMeetingHandler(cfg, sessions, meetingStore, roomState, nil, nil, nil, nil)

	meeting, _, err := meetingStore.CreateMeeting("repo-host-leave", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create repo-backed meeting failed: %v", err)
	}
	if _, _, _, err = meetingStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join repo-backed meeting u1002 failed: %v", err)
	}
	if _, _, _, err = meetingStore.JoinMeeting(meeting.ID, "", "u1003"); err != nil {
		t.Fatalf("join repo-backed meeting u1003 failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 45, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 46, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateInMeeting)

	thirdSess, thirdConn, thirdCleanup := newRunningSession(t, 47, cfg)
	defer thirdCleanup()
	sessions.Add(thirdSess)
	sessions.BindUser(thirdSess, "u1003")
	thirdSess.SetMeetingID(meeting.ID)
	thirdSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(hostSess, payload)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected host leave rsp type: got %v want %v", hostType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(hostPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal host leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected repo-backed host leave success")
	}

	peerType1, peerPayload1 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected peer leave notify type: got %v want %v", peerType1, protocol.MeetParticipantLeave)
	}
	var peerLeave protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(peerPayload1, &peerLeave); err != nil {
		t.Fatalf("unmarshal peer leave notify failed: %v", err)
	}
	if peerLeave.UserId != "u1001" || peerLeave.Reason != "主动离开" {
		t.Fatalf("unexpected repo-backed peer leave notify: %+v", peerLeave)
	}

	peerType2, peerPayload2 := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected peer host-changed type: got %v want %v", peerType2, protocol.MeetHostChanged)
	}
	var hostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(peerPayload2, &hostChanged); err != nil {
		t.Fatalf("unmarshal repo-backed host changed notify failed: %v", err)
	}
	if hostChanged.NewHostId != "u1002" {
		t.Fatalf("expected repo-backed transfer to u1002, got %+v", hostChanged)
	}

	thirdType1, _ := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType1 != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected third leave notify type: got %v want %v", thirdType1, protocol.MeetParticipantLeave)
	}
	thirdType2, thirdPayload2 := readFrame(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdType2 != protocol.MeetHostChanged {
		t.Fatalf("unexpected third host-changed type: got %v want %v", thirdType2, protocol.MeetHostChanged)
	}
	var thirdHostChanged protocol.MeetHostChangedNotifyBody
	if err := proto.Unmarshal(thirdPayload2, &thirdHostChanged); err != nil {
		t.Fatalf("unmarshal third host changed notify failed: %v", err)
	}
	if thirdHostChanged.NewHostId != "u1002" {
		t.Fatalf("expected repo-backed host change for all peers, got %+v", thirdHostChanged)
	}

	peerStateSync := readMeetStateSync(t, peerConn, cfg.MaxPayloadBytes)
	if peerStateSync.HostId != "u1002" {
		t.Fatalf("unexpected repo-backed peer host id in state sync: %+v", peerStateSync)
	}
	if got := participantIDs(peerStateSync.Participants); len(got) != 2 || got[0] != "u1002" || got[1] != "u1003" {
		t.Fatalf("unexpected repo-backed peer state sync participants: %+v", got)
	}

	thirdStateSync := readMeetStateSync(t, thirdConn, cfg.MaxPayloadBytes)
	if thirdStateSync.HostId != "u1002" {
		t.Fatalf("unexpected repo-backed third host id in state sync: %+v", thirdStateSync)
	}
	if got := participantIDs(thirdStateSync.Participants); len(got) != 2 || got[0] != "u1002" || got[1] != "u1003" {
		t.Fatalf("unexpected repo-backed third state sync participants: %+v", got)
	}

	if isHost, err := meetingStore.IsMeetingHost(meeting.ID, "u1002"); err != nil || !isHost {
		t.Fatalf("expected repo-backed store to mark u1002 as host, isHost=%v err=%v", isHost, err)
	}
	if !meetingRepo.called("transfer_host") || !participantRepo.called("mark_left") {
		t.Fatalf("expected transfer_host and mark_left calls, meetingCalls=%+v participantCalls=%+v", meetingRepo.calls, participantRepo.calls)
	}
	if _, err := memStore.Participants(meeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected repo-backed room to stay out of memory store, got %v", err)
	}

	cleanupMeeting, _, err := meetingStore.CreateMeeting("repo-cleanup-room", "", "u1003", 1)
	if err != nil {
		t.Fatalf("create repo-backed cleanup meeting failed: %v", err)
	}
	lastSess, lastConn, lastCleanup := newRunningSession(t, 48, cfg)
	defer lastCleanup()
	sessions.Add(lastSess)
	sessions.BindUser(lastSess, "u1003")
	lastSess.SetMeetingID(cleanupMeeting.ID)
	lastSess.SetState(server.StateInMeeting)

	h.HandleLeave(lastSess, payload)

	lastType, lastPayload := readFrame(t, lastConn, cfg.MaxPayloadBytes)
	if lastType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected cleanup leave rsp type: got %v want %v", lastType, protocol.MeetLeaveRsp)
	}
	if err := proto.Unmarshal(lastPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal repo-backed cleanup leave rsp failed: %v", err)
	}
	if !leaveRsp.Success {
		t.Fatalf("expected repo-backed cleanup leave success")
	}
	if !meetingRepo.called("delete_meeting") {
		t.Fatalf("expected repo-backed delete_meeting call, meetingCalls=%+v", meetingRepo.calls)
	}
	if _, err := meetingStore.HasParticipant(cleanupMeeting.ID, "u1003"); err == nil {
		t.Fatalf("expected cleaned repo-backed meeting to be gone")
	}
	if _, err := memStore.Participants(cleanupMeeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected repo-backed cleanup room to stay out of memory store, got %v", err)
	}
}

func TestMeetingHandlerLeaveFailureKeepsSessionState(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	h := NewMeetingHandler(cfg, sessions, failingLeaveMeetingStore{err: errors.New("db unavailable")}, roomState, nil, nil, nil, nil)

	hostSess, hostConn, hostCleanup := newRunningSession(t, 61, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID("m1001")
	hostSess.SetState(server.StateInMeeting)

	peerSess, peerConn, peerCleanup := newRunningSession(t, 62, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID("m1001")
	peerSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MeetLeaveReqBody{})
	if err != nil {
		t.Fatalf("marshal leave req failed: %v", err)
	}

	h.HandleLeave(peerSess, payload)

	peerType, peerPayload := readFrame(t, peerConn, cfg.MaxPayloadBytes)
	if peerType != protocol.MeetLeaveRsp {
		t.Fatalf("unexpected peer leave rsp type: got %v want %v", peerType, protocol.MeetLeaveRsp)
	}
	var leaveRsp protocol.MeetLeaveRspBody
	if err := proto.Unmarshal(peerPayload, &leaveRsp); err != nil {
		t.Fatalf("unmarshal leave rsp failed: %v", err)
	}
	if leaveRsp.Success {
		t.Fatalf("expected leave failure response")
	}

	if peerSess.State() != server.StateInMeeting || peerSess.MeetingID() != "m1001" {
		t.Fatalf("expected leaving session to stay in meeting on failure, state=%v meeting=%q", peerSess.State(), peerSess.MeetingID())
	}

	assertNoFrame(t, hostConn, "host connection on leave failure")
}

func TestRouterOnSessionClosedTriggersDisconnectLeave(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, _, err := memStore.CreateMeeting("disconnect-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 51, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, _, peerCleanup := newRunningSession(t, 52, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateDisconnected)

	router.OnSessionClosed(peerSess)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected disconnect notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal disconnect leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "网络断开" {
		t.Fatalf("unexpected disconnect leave notify: %+v", leaveNotify)
	}

	if peerSess.State() != server.StateDisconnected {
		t.Fatalf("expected disconnected session state to be preserved, got %v", peerSess.State())
	}
	if peerSess.MeetingID() != "" {
		t.Fatalf("expected disconnected session meeting to be cleared, got %q", peerSess.MeetingID())
	}
	if exists, err := memStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected disconnected participant removed, exists=%v err=%v", exists, err)
	}
}

func TestRouterOnSessionClosedUsesRepoBackedMeetingStore(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)

	meetingStore, meetingRepo, participantRepo := newRepoBackedMeetingStore(memStore)
	router := NewRouter(cfg, sessions, memStore, meetingStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, _, err := meetingStore.CreateMeeting("repo-disconnect-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create repo-backed meeting failed: %v", err)
	}
	if _, _, _, err = meetingStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join repo-backed meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 251, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, _, peerCleanup := newRunningSession(t, 252, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateDisconnected)

	router.OnSessionClosed(peerSess)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected disconnect notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal disconnect notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "网络断开" {
		t.Fatalf("unexpected disconnect notify payload: %+v", leaveNotify)
	}

	if peerSess.State() != server.StateDisconnected || peerSess.MeetingID() != "" {
		t.Fatalf("expected disconnected session state preserved with cleared meeting, state=%v meeting=%q", peerSess.State(), peerSess.MeetingID())
	}
	if exists, err := meetingStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected repo-backed participant removed, exists=%v err=%v", exists, err)
	}
	if !meetingRepo.called("find_by_meeting_no") || !participantRepo.called("mark_left") {
		t.Fatalf("expected repo-backed store to handle disconnect leave, meetingCalls=%+v participantCalls=%+v", meetingRepo.calls, participantRepo.calls)
	}
	if _, err := memStore.Participants(meeting.ID); err != store.ErrMeetingNotFound {
		t.Fatalf("expected router to avoid memory store fallback, got %v", err)
	}
}

func TestRouterOnSessionClosedLogoutUsesIntentionalLeaveReason(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, _, err := memStore.CreateMeeting("logout-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err = memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	hostSess, hostConn, hostCleanup := newRunningSession(t, 301, cfg)
	defer hostCleanup()
	sessions.Add(hostSess)
	sessions.BindUser(hostSess, "u1001")
	hostSess.SetMeetingID(meeting.ID)
	hostSess.SetState(server.StateInMeeting)

	peerSess, _, peerCleanup := newRunningSession(t, 302, cfg)
	defer peerCleanup()
	sessions.Add(peerSess)
	sessions.BindUser(peerSess, "u1002")
	peerSess.SetMeetingID(meeting.ID)
	peerSess.SetState(server.StateDisconnected)
	peerSess.MarkLogoutClose()

	router.OnSessionClosed(peerSess)

	hostType, hostPayload := readFrame(t, hostConn, cfg.MaxPayloadBytes)
	if hostType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected logout notify type: got %v want %v", hostType, protocol.MeetParticipantLeave)
	}
	var leaveNotify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(hostPayload, &leaveNotify); err != nil {
		t.Fatalf("unmarshal logout leave notify failed: %v", err)
	}
	if leaveNotify.UserId != "u1002" || leaveNotify.Reason != "主动离开" {
		t.Fatalf("unexpected logout leave notify: %+v", leaveNotify)
	}

	if peerSess.State() != server.StateDisconnected || peerSess.MeetingID() != "" {
		t.Fatalf("expected logout session to stay disconnected with cleared meeting, state=%v meeting=%q", peerSess.State(), peerSess.MeetingID())
	}
	if exists, err := memStore.HasParticipant(meeting.ID, "u1002"); err != nil || exists {
		t.Fatalf("expected logout participant removed, exists=%v err=%v", exists, err)
	}
}

func TestRouterOnSessionClosedClearsSessionPresence(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sessionStore := newRecordingSessionStore()
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, sessionStore, tokenManager, limiter, nil, nil)

	sess, _, cleanup := newRunningSession(t, 401, cfg)
	defer cleanup()
	sessions.Add(sess)
	sessions.BindUser(sess, "u9001")
	sess.SetState(server.StateDisconnected)

	router.OnSessionClosed(sess)

	if len(sessionStore.deletes) != 1 {
		t.Fatalf("expected one session delete on close, got %d", len(sessionStore.deletes))
	}
	if got := sessionStore.deletes[0]; got.userID != "u9001" || got.sessionID != sess.ID {
		t.Fatalf("unexpected close delete record: %+v", got)
	}
}

func TestRouterOnUserNodeEventAppliesRemoteKickControl(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-b"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sessionStore := newRecordingSessionStore()
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, sessionStore, tokenManager, limiter, nil, nil)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 402, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-remote")
	targetSess.SetState(server.StateInMeeting)

	nextState := int32(server.StateAuthenticated)
	router.OnUserNodeEvent(store.UserNodeEvent{
		TargetUserID:    "u1002",
		TargetSessionID: targetSess.ID,
		Frame:           protocol.EncodeFrame(protocol.MeetParticipantLeave, mustMarshalProto(&protocol.MeetParticipantLeaveNotifyBody{UserId: "u1002", Reason: "被主持人移出会议"})),
		ResetMeeting:    true,
		State:           &nextState,
	})

	msgType, payload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected remote control frame type: got %v want %v", msgType, protocol.MeetParticipantLeave)
	}
	var notify protocol.MeetParticipantLeaveNotifyBody
	if err := proto.Unmarshal(payload, &notify); err != nil {
		t.Fatalf("unmarshal remote control notify failed: %v", err)
	}
	if notify.UserId != "u1002" || notify.Reason != "被主持人移出会议" {
		t.Fatalf("unexpected remote control notify: %+v", notify)
	}
	if targetSess.MeetingID() != "" || targetSess.State() != server.StateAuthenticated {
		t.Fatalf("expected target session reset by remote control, state=%v meeting=%q", targetSess.State(), targetSess.MeetingID())
	}
	if len(sessionStore.upserts) == 0 {
		t.Fatal("expected remote control to sync session presence")
	}
	if got := sessionStore.upserts[len(sessionStore.upserts)-1]; got.UserID != "u1002" || got.MeetingID != "" || got.Status != int32(server.StateAuthenticated) {
		t.Fatalf("unexpected remote control session presence: %+v", got)
	}
}
