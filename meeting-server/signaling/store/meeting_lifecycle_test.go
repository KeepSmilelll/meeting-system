package store

import (
	"context"
	"sort"
	"sync"
	"testing"
	"time"

	"meeting-server/signaling/model"
)

type fakeMeetingRepo struct {
	mu           sync.Mutex
	nextID       uint64
	meetingsByNo map[string]*model.Meeting
	meetingsByID map[uint64]string
	participants map[uint64]map[uint64]*model.Participant
	calls        []string
}

func newFakeMeetingRepo() *fakeMeetingRepo {
	return &fakeMeetingRepo{
		nextID:       1,
		meetingsByNo: make(map[string]*model.Meeting),
		meetingsByID: make(map[uint64]string),
		participants: make(map[uint64]map[uint64]*model.Participant),
	}
}

func (r *fakeMeetingRepo) record(call string) {
	r.calls = append(r.calls, call)
}

func (r *fakeMeetingRepo) FindByID(ctx context.Context, meetingID uint64) (*model.Meeting, error) {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	meetingNo, ok := r.meetingsByID[meetingID]
	if !ok {
		return nil, ErrMeetingNotFound
	}
	meeting := *r.meetingsByNo[meetingNo]
	return &meeting, nil
}

func (r *fakeMeetingRepo) FindByMeetingNo(ctx context.Context, meetingNo string) (*model.Meeting, error) {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	meeting, ok := r.meetingsByNo[meetingNo]
	if !ok {
		return nil, ErrMeetingNotFound
	}
	copy := *meeting
	return &copy, nil
}

func (r *fakeMeetingRepo) CreateMeeting(ctx context.Context, meeting *model.Meeting) error {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	r.record("create_meeting")
	copy := *meeting
	copy.ID = r.nextID
	r.nextID++
	r.meetingsByNo[copy.MeetingNo] = &copy
	r.meetingsByID[copy.ID] = copy.MeetingNo
	meeting.ID = copy.ID
	return nil
}

func (r *fakeMeetingRepo) AddParticipant(ctx context.Context, participant *model.Participant) error {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	r.record("add_participant")
	bucket, ok := r.participants[participant.MeetingID]
	if !ok {
		bucket = make(map[uint64]*model.Participant)
		r.participants[participant.MeetingID] = bucket
	}
	copy := *participant
	if copy.JoinedAt.IsZero() {
		copy.JoinedAt = time.Now().UTC()
	}
	bucket[copy.UserID] = &copy
	return nil
}

func (r *fakeMeetingRepo) MarkParticipantLeft(ctx context.Context, meetingID, userID uint64) error {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	r.record("mark_left")
	bucket, ok := r.participants[meetingID]
	if !ok {
		return ErrMeetingNotFound
	}
	participant, ok := bucket[userID]
	if !ok {
		return ErrMeetingNotFound
	}
	leftAt := time.Now().UTC()
	participant.LeftAt = &leftAt
	return nil
}

func (r *fakeMeetingRepo) ListActiveParticipants(ctx context.Context, meetingID uint64) ([]model.Participant, error) {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	bucket := r.participants[meetingID]
	result := make([]model.Participant, 0, len(bucket))
	for _, participant := range bucket {
		if participant.LeftAt != nil {
			continue
		}
		result = append(result, *participant)
	}
	sort.Slice(result, func(i, j int) bool {
		if result[i].JoinedAt.Equal(result[j].JoinedAt) {
			return result[i].ID < result[j].ID
		}
		return result[i].JoinedAt.Before(result[j].JoinedAt)
	})
	return result, nil
}

func (r *fakeMeetingRepo) TransferHost(ctx context.Context, meetingID uint64) (*model.Participant, error) {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	r.record("transfer_host")
	bucket := r.participants[meetingID]
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
		return nil, ErrMeetingNotFound
	}
	best.Role = 1
	if current, ok := bucket[best.UserID]; ok {
		current.Role = 1
	}
	if meetingNo, ok := r.meetingsByID[meetingID]; ok {
		if meeting, ok := r.meetingsByNo[meetingNo]; ok {
			meeting.HostUserID = best.UserID
		}
	}
	return best, nil
}

func (r *fakeMeetingRepo) DeleteMeeting(ctx context.Context, meetingID uint64) error {
	_ = ctx
	r.mu.Lock()
	defer r.mu.Unlock()
	r.record("delete_meeting")
	meetingNo, ok := r.meetingsByID[meetingID]
	if !ok {
		return ErrMeetingNotFound
	}
	delete(r.meetingsByID, meetingID)
	delete(r.meetingsByNo, meetingNo)
	delete(r.participants, meetingID)
	return nil
}

func TestMeetingLifecycleStoreRepoPathCreateJoinLeaveHostTransfer(t *testing.T) {
	fallback := NewMemoryStore()
	repo := newFakeMeetingRepo()
	store := NewMeetingLifecycleStore(fallback, repo)

	meeting, host, err := store.CreateMeeting("repo-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if meeting == nil || meeting.ID == "" {
		t.Fatalf("expected created meeting id, got %+v", meeting)
	}
	if host == nil || host.UserId != "u1001" || host.Role != 1 {
		t.Fatalf("unexpected host participant: %+v", host)
	}

	created, err := repo.FindByMeetingNo(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("repo lookup failed: %v", err)
	}
	if created.HostUserID != 1001 {
		t.Fatalf("expected uint64 host id 1001, got %d", created.HostUserID)
	}

	if _, _, joined, err := store.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join u1002 failed: %v", err)
	} else if joined == nil || joined.UserId != "u1002" {
		t.Fatalf("unexpected joined participant: %+v", joined)
	}
	if _, _, joined, err := store.JoinMeeting(meeting.ID, "", "u1003"); err != nil {
		t.Fatalf("join u1003 failed: %v", err)
	} else if joined == nil || joined.UserId != "u1003" {
		t.Fatalf("unexpected joined participant: %+v", joined)
	}

	active, err := repo.ListActiveParticipants(context.Background(), created.ID)
	if err != nil {
		t.Fatalf("list active participants failed: %v", err)
	}
	if len(active) != 3 {
		t.Fatalf("expected 3 active participants, got %d", len(active))
	}

	hostChanged, newHost, remaining, err := store.LeaveMeeting(meeting.ID, "u1001")
	if err != nil {
		t.Fatalf("leave host failed: %v", err)
	}
	if !hostChanged || newHost == nil || newHost.UserId != "u1002" {
		t.Fatalf("expected host transfer to u1002, got changed=%v newHost=%+v", hostChanged, newHost)
	}
	if remaining != 2 {
		t.Fatalf("expected 2 remaining participants, got %d", remaining)
	}
	if newHost.Role != 1 {
		t.Fatalf("expected new host role=1, got %+v", newHost)
	}
	created, err = repo.FindByMeetingNo(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("repo lookup after transfer failed: %v", err)
	}
	if created.HostUserID != 1002 {
		t.Fatalf("expected host user id 1002 after transfer, got %d", created.HostUserID)
	}

	if isHost, err := store.IsMeetingHost(meeting.ID, "u1002"); err != nil || !isHost {
		t.Fatalf("expected u1002 to be host, isHost=%v err=%v", isHost, err)
	}
	if exists, err := store.HasParticipant(meeting.ID, "u1003"); err != nil || !exists {
		t.Fatalf("expected u1003 to remain in meeting, exists=%v err=%v", exists, err)
	}

	if _, _, remaining, err := store.LeaveMeeting(meeting.ID, "u1002"); err != nil {
		t.Fatalf("leave new host failed: %v", err)
	} else if remaining != 1 {
		t.Fatalf("expected 1 remaining participant, got %d", remaining)
	}
	if _, _, remaining, err := store.LeaveMeeting(meeting.ID, "u1003"); err != nil {
		t.Fatalf("leave last participant failed: %v", err)
	} else if remaining != 0 {
		t.Fatalf("expected room cleanup with 0 remaining, got %d", remaining)
	}
	if _, err := repo.FindByMeetingNo(context.Background(), meeting.ID); err == nil {
		t.Fatalf("expected meeting to be deleted from repo")
	}
}
