package store

import (
	"context"
	"testing"
	"time"

	"meeting-server/signaling/model"
)

func TestMessageRepoSaveListAndSearchInMemory(t *testing.T) {
	repo := NewInMemoryMessageRepo()

	m1 := &model.Message{MeetingID: 1001, SenderID: 2001, Type: 1, Content: "hello world"}
	m2 := &model.Message{MeetingID: 1001, SenderID: 2002, Type: 1, Content: "keyword message"}
	m3 := &model.Message{MeetingID: 1001, SenderID: 2003, Type: 1, Content: "another keyword"}

	if err := repo.Save(context.Background(), m1); err != nil {
		t.Fatalf("save m1 failed: %v", err)
	}
	if err := repo.Save(context.Background(), m2); err != nil {
		t.Fatalf("save m2 failed: %v", err)
	}
	if err := repo.Save(context.Background(), m3); err != nil {
		t.Fatalf("save m3 failed: %v", err)
	}

	listed, err := repo.ListByMeeting(context.Background(), 1001, 2)
	if err != nil {
		t.Fatalf("list failed: %v", err)
	}
	if len(listed) != 2 {
		t.Fatalf("unexpected list size: got %d want 2", len(listed))
	}
	if listed[0].Content != "keyword message" || listed[1].Content != "another keyword" {
		t.Fatalf("unexpected list order/content: %+v", listed)
	}

	matched, err := repo.SearchByContent(context.Background(), 1001, "keyword", 10)
	if err != nil {
		t.Fatalf("search failed: %v", err)
	}
	if len(matched) != 2 {
		t.Fatalf("unexpected search size: got %d want 2", len(matched))
	}
}

func TestMessageRepoListByMeetingBeforeTimestamp(t *testing.T) {
	repo := NewInMemoryMessageRepo()
	base := time.UnixMilli(1_700_000_000_000).UTC()

	for i := 0; i < 55; i++ {
		message := &model.Message{
			MeetingID: 1001,
			SenderID:  2001,
			Type:      1,
			Content:   "page message",
			CreatedAt: base.Add(time.Duration(i) * time.Millisecond),
		}
		if err := repo.Save(context.Background(), message); err != nil {
			t.Fatalf("save message %d failed: %v", i, err)
		}
	}

	latest, err := repo.ListByMeeting(context.Background(), 1001, 50)
	if err != nil {
		t.Fatalf("list latest failed: %v", err)
	}
	if len(latest) != 50 {
		t.Fatalf("unexpected latest size: got %d want 50", len(latest))
	}
	if latest[0].CreatedAt != base.Add(5*time.Millisecond) || latest[49].CreatedAt != base.Add(54*time.Millisecond) {
		t.Fatalf("unexpected latest page bounds: first=%s last=%s", latest[0].CreatedAt, latest[49].CreatedAt)
	}

	older, err := repo.ListByMeeting(context.Background(), 1001, 50, latest[0].CreatedAt.UnixMilli())
	if err != nil {
		t.Fatalf("list older failed: %v", err)
	}
	if len(older) != 5 {
		t.Fatalf("unexpected older size: got %d want 5", len(older))
	}
	if older[0].CreatedAt != base || older[4].CreatedAt != base.Add(4*time.Millisecond) {
		t.Fatalf("unexpected older page bounds: first=%s last=%s", older[0].CreatedAt, older[4].CreatedAt)
	}
}

func TestMessageRepoRejectsInvalidPayload(t *testing.T) {
	repo := NewInMemoryMessageRepo()

	err := repo.Save(context.Background(), &model.Message{MeetingID: 0, SenderID: 100, Content: "x"})
	if err == nil {
		t.Fatalf("expected invalid payload error")
	}

	_, err = repo.ListByMeeting(context.Background(), 0, 10)
	if err == nil {
		t.Fatalf("expected invalid meeting id error")
	}
}
