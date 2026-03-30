package auth

import (
	"testing"
	"time"
)

func TestTokenGenerateVerify(t *testing.T) {
	m := NewTokenManager("secret", time.Minute)
	token, err := m.Generate("u1001")
	if err != nil {
		t.Fatalf("Generate failed: %v", err)
	}

	uid, err := m.Verify(token)
	if err != nil {
		t.Fatalf("Verify failed: %v", err)
	}
	if uid != "u1001" {
		t.Fatalf("unexpected user id: %s", uid)
	}
}

func TestTokenExpired(t *testing.T) {
	m := NewTokenManager("secret", 10*time.Millisecond)
	token, err := m.Generate("u1001")
	if err != nil {
		t.Fatalf("Generate failed: %v", err)
	}

	time.Sleep(20 * time.Millisecond)
	if _, err := m.Verify(token); err == nil {
		t.Fatalf("expected expired token error")
	}
}
