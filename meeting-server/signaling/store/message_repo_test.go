package store

import (
	"context"
	"testing"

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
