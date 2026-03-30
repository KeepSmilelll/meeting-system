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
    if joined.UserID != "u1002" {
        t.Fatalf("unexpected joined user: %s", joined.UserID)
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

