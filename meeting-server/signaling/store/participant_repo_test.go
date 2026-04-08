package store

import (
	"context"
	"testing"
	"time"

	"meeting-server/signaling/model"
)

func TestParticipantRepoLifecycle(t *testing.T) {
	repo := NewInMemoryParticipantRepo()
	ctx := context.Background()

	firstJoined := time.Date(2026, 3, 31, 10, 0, 0, 0, time.UTC)
	secondJoined := time.Date(2026, 3, 31, 10, 1, 0, 0, time.UTC)

	first := &model.Participant{MeetingID: 1001, UserID: 2001, JoinedAt: firstJoined}
	second := &model.Participant{MeetingID: 1001, UserID: 2002, JoinedAt: secondJoined}
	third := &model.Participant{MeetingID: 1001, UserID: 2003, JoinedAt: secondJoined}

	if err := repo.AddParticipant(ctx, first); err != nil {
		t.Fatalf("add first participant: %v", err)
	}
	if err := repo.AddParticipant(ctx, second); err != nil {
		t.Fatalf("add second participant: %v", err)
	}
	if err := repo.AddParticipant(ctx, third); err != nil {
		t.Fatalf("add third participant: %v", err)
	}

	active, err := repo.ListActiveParticipants(ctx, 1001)
	if err != nil {
		t.Fatalf("list active participants: %v", err)
	}
	if len(active) != 3 {
		t.Fatalf("expected 3 active participants, got %d", len(active))
	}
	if active[0].UserID != 2001 || active[1].UserID != 2002 || active[2].UserID != 2003 {
		t.Fatalf("unexpected active ordering: %+v", active)
	}

	found, err := repo.FindActiveParticipant(ctx, 1001, 2002)
	if err != nil {
		t.Fatalf("find active participant: %v", err)
	}
	if found.UserID != 2002 || found.MeetingID != 1001 {
		t.Fatalf("unexpected participant returned: %+v", found)
	}

	if err := repo.MarkParticipantLeft(ctx, 1001, 2002); err != nil {
		t.Fatalf("mark participant left: %v", err)
	}

	active, err = repo.ListActiveParticipants(ctx, 1001)
	if err != nil {
		t.Fatalf("list active participants after leave: %v", err)
	}
	if len(active) != 2 {
		t.Fatalf("expected 2 active participants after leave, got %d", len(active))
	}
	if active[0].UserID != 2001 || active[1].UserID != 2003 {
		t.Fatalf("unexpected active ordering after leave: %+v", active)
	}

	if _, err := repo.FindActiveParticipant(ctx, 1001, 2002); err == nil {
		t.Fatalf("expected find active participant to fail after leave")
	}
}

func TestParticipantRepoPickNextHostCandidate(t *testing.T) {
	repo := NewInMemoryParticipantRepo()
	ctx := context.Background()

	base := time.Date(2026, 3, 31, 10, 0, 0, 0, time.UTC)
	participants := []*model.Participant{
		{MeetingID: 2001, UserID: 3009, JoinedAt: base.Add(2 * time.Minute)},
		{MeetingID: 2001, UserID: 3003, JoinedAt: base},
		{MeetingID: 2001, UserID: 3007, JoinedAt: base},
	}

	for _, participant := range participants {
		if err := repo.AddParticipant(ctx, participant); err != nil {
			t.Fatalf("add participant %d: %v", participant.UserID, err)
		}
	}

	candidate, err := repo.PickNextHostCandidate(ctx, 2001)
	if err != nil {
		t.Fatalf("pick next host candidate: %v", err)
	}
	if candidate.UserID != 3003 {
		t.Fatalf("expected smallest ID among earliest joined participants, got user_id=%d", candidate.UserID)
	}
}

func TestParticipantRepoEmptyMeeting(t *testing.T) {
	repo := NewInMemoryParticipantRepo()
	ctx := context.Background()

	active, err := repo.ListActiveParticipants(ctx, 9999)
	if err != nil {
		t.Fatalf("list active participants for empty meeting: %v", err)
	}
	if len(active) != 0 {
		t.Fatalf("expected empty participant list, got %d", len(active))
	}
	if _, err := repo.PickNextHostCandidate(ctx, 9999); err == nil {
		t.Fatalf("expected candidate lookup to fail for empty meeting")
	}
}
