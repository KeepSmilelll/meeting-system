package store

import (
	"context"
	"testing"

	"meeting-server/signaling/auth"
	"meeting-server/signaling/model"

	"gorm.io/gorm"
)

type fakeUserRepo struct {
	byUsername map[string]*model.User
	byID       map[uint64]*model.User
}

func newFakeUserRepo() *fakeUserRepo {
	return &fakeUserRepo{
		byUsername: make(map[string]*model.User),
		byID:       make(map[uint64]*model.User),
	}
}

func (r *fakeUserRepo) FindByUsername(_ context.Context, username string) (*model.User, error) {
	user, ok := r.byUsername[username]
	if !ok {
		return nil, gorm.ErrRecordNotFound
	}
	copy := *user
	return &copy, nil
}

func (r *fakeUserRepo) FindByID(_ context.Context, userID uint64) (*model.User, error) {
	user, ok := r.byID[userID]
	if !ok {
		return nil, gorm.ErrRecordNotFound
	}
	copy := *user
	return &copy, nil
}

func (r *fakeUserRepo) Create(_ context.Context, user *model.User) error {
	copy := *user
	r.byUsername[copy.Username] = &copy
	r.byID[copy.ID] = &copy
	return nil
}

func TestMeetingLifecycle(t *testing.T) {
	s := NewMemoryStore()

	user, err := s.Authenticate("demo", "demo")
	if err != nil {
		t.Fatalf("authenticate failed: %v", err)
	}

	meeting, _, err := s.CreateMeeting("test", "", user.ID, 2)
	if err != nil {
		t.Fatalf("CreateMeeting failed: %v", err)
	}

	_, participants, joined, err := s.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("JoinMeeting failed: %v", err)
	}
	if joined.UserId != "u1002" {
		t.Fatalf("unexpected joined user: %s", joined.UserId)
	}
	if len(participants) != 2 {
		t.Fatalf("expected 2 participants, got %d", len(participants))
	}

	_, _, _, err = s.JoinMeeting(meeting.ID, "", "u1003")
	if err != ErrMeetingFull {
		t.Fatalf("expected ErrMeetingFull, got: %v", err)
	}

	_, _, remaining, err := s.LeaveMeeting(meeting.ID, "u1002")
	if err != nil {
		t.Fatalf("LeaveMeeting failed: %v", err)
	}
	if remaining != 1 {
		t.Fatalf("expected 1 remaining user, got %d", remaining)
	}
}

func TestMemoryStoreParticipantMediaSsrcSnapshot(t *testing.T) {
	s := NewMemoryStore()

	meeting, host, err := s.CreateMeeting("ssrc-snapshot", "", "u1001", 4)
	if err != nil {
		t.Fatalf("CreateMeeting failed: %v", err)
	}
	if err := s.SetParticipantMediaSsrc(meeting.ID, host.UserId, 1111, 2222); err != nil {
		t.Fatalf("SetParticipantMediaSsrc failed: %v", err)
	}

	_, participants, err := s.SnapshotMeeting(meeting.ID)
	if err != nil {
		t.Fatalf("SnapshotMeeting failed: %v", err)
	}
	if len(participants) != 1 || participants[0] == nil {
		t.Fatalf("expected one participant snapshot, got %+v", participants)
	}
	if participants[0].AudioSsrc != 1111 || participants[0].VideoSsrc != 2222 {
		t.Fatalf("unexpected participant SSRC snapshot: %+v", participants[0])
	}
}

func TestMemoryStoreDefaultUserUsesArgon2idHash(t *testing.T) {
	s := NewMemoryStore()

	demo, ok := s.GetUserByID("u1001")
	if !ok {
		t.Fatalf("expected default user u1001")
	}
	if !auth.IsArgon2idHash(demo.PasswordHash) {
		t.Fatalf("expected argon2id hash, got %q", demo.PasswordHash)
	}

	if _, err := s.Authenticate("demo", "demo"); err != nil {
		t.Fatalf("expected argon2id verification for plain password input, got %v", err)
	}
	if _, err := s.Authenticate("demo", demo.PasswordHash); err == nil {
		t.Fatalf("expected direct hash input to be rejected")
	}
}

func TestMemoryStoreAuthenticatePrefersRepoUser(t *testing.T) {
	s := NewMemoryStore()
	repo := newFakeUserRepo()
	hasher := auth.NewPasswordHasher()
	repoHash, err := hasher.HashPassword("repo-secret")
	if err != nil {
		t.Fatalf("hash repo secret failed: %v", err)
	}

	repo.byUsername["demo"] = &model.User{ID: 2001, Username: "demo", DisplayName: "Repo Demo", PasswordHash: repoHash}
	repo.byID[2001] = repo.byUsername["demo"]
	s.SetUserRepo(repo)

	user, err := s.Authenticate("demo", "repo-secret")
	if err != nil {
		t.Fatalf("repo authenticate failed: %v", err)
	}
	if user.ID != "u2001" {
		t.Fatalf("expected repo user id u2001, got %s", user.ID)
	}

	if _, err := s.Authenticate("demo", "demo"); err == nil {
		t.Fatal("expected local plaintext password to fail when repo user exists")
	}
	if _, err := s.Authenticate("demo", repoHash); err == nil {
		t.Fatal("expected direct hash input to fail for repo user")
	}

	got, ok := s.GetUserByID("u2001")
	if !ok || got.Username != "demo" {
		t.Fatalf("expected repo get-by-id success, got ok=%v user=%+v", ok, got)
	}
}

func TestMemoryStoreSeedDefaultUsersToRepo(t *testing.T) {
	s := NewMemoryStore()
	repo := newFakeUserRepo()
	s.SetUserRepo(repo)

	if err := s.SeedDefaultUsersToRepo(context.Background()); err != nil {
		t.Fatalf("SeedDefaultUsersToRepo failed: %v", err)
	}

	for _, username := range []string{"demo", "alice", "bob"} {
		seeded, ok := repo.byUsername[username]
		if !ok {
			t.Fatalf("expected seeded user %s", username)
		}
		if !auth.IsArgon2idHash(seeded.PasswordHash) {
			t.Fatalf("expected argon2 hash for %s, got %q", username, seeded.PasswordHash)
		}
	}

	if _, err := s.Authenticate("demo", "demo"); err != nil {
		t.Fatalf("expected seeded repo auth to accept demo password, got %v", err)
	}
}

func TestLeaveMeetingTransfersHostByJoinOrder(t *testing.T) {
	s := NewMemoryStore()

	meeting, _, err := s.CreateMeeting("host-transfer", "", "u1001", 4)
	if err != nil {
		t.Fatalf("CreateMeeting failed: %v", err)
	}
	if _, _, _, err := s.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("JoinMeeting u1002 failed: %v", err)
	}
	if _, _, _, err := s.JoinMeeting(meeting.ID, "", "u1003"); err != nil {
		t.Fatalf("JoinMeeting u1003 failed: %v", err)
	}

	hostChanged, newHost, remaining, err := s.LeaveMeeting(meeting.ID, "u1001")
	if err != nil {
		t.Fatalf("LeaveMeeting failed: %v", err)
	}
	if !hostChanged {
		t.Fatalf("expected host transfer")
	}
	if remaining != 2 {
		t.Fatalf("expected 2 remaining users, got %d", remaining)
	}
	if newHost == nil || newHost.UserId != "u1002" {
		t.Fatalf("expected earliest joined participant u1002 to become host, got %+v", newHost)
	}

	isHost, err := s.IsMeetingHost(meeting.ID, "u1002")
	if err != nil {
		t.Fatalf("IsMeetingHost failed: %v", err)
	}
	if !isHost {
		t.Fatalf("expected u1002 to be host after transfer")
	}
}

func TestLeaveMeetingDeletesEmptyRoom(t *testing.T) {
	s := NewMemoryStore()

	meeting, _, err := s.CreateMeeting("cleanup", "", "u1001", 1)
	if err != nil {
		t.Fatalf("CreateMeeting failed: %v", err)
	}

	hostChanged, newHost, remaining, err := s.LeaveMeeting(meeting.ID, "u1001")
	if err != nil {
		t.Fatalf("LeaveMeeting failed: %v", err)
	}
	if hostChanged || newHost != nil {
		t.Fatalf("expected no host transfer on empty room cleanup")
	}
	if remaining != 0 {
		t.Fatalf("expected 0 remaining users, got %d", remaining)
	}

	if _, err := s.Participants(meeting.ID); err != ErrMeetingNotFound {
		t.Fatalf("expected meeting to be deleted, got %v", err)
	}
}
