package store

import (
	"testing"
	"time"

	"meeting-server/signaling/model"
)

func TestPickNextHostCandidate(t *testing.T) {
	base := time.Date(2026, 3, 31, 10, 0, 0, 0, time.UTC)

	participants := []model.Participant{
		{ID: 9, UserID: 9009, JoinedAt: base.Add(2 * time.Minute)},
		{ID: 3, UserID: 9003, JoinedAt: base},
		{ID: 7, UserID: 9007, JoinedAt: base},
	}

	got := pickNextHostCandidate(participants)
	if got == nil {
		t.Fatalf("expected candidate, got nil")
	}
	if got.UserID != 9003 {
		t.Fatalf("expected smallest ID among earliest joined participants, got user_id=%d", got.UserID)
	}
}

func TestPickNextHostCandidateEmpty(t *testing.T) {
	if got := pickNextHostCandidate(nil); got != nil {
		t.Fatalf("expected nil candidate, got %#v", got)
	}
}
