package store

import "testing"

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
