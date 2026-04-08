package auth

import (
	"testing"
	"time"
)

func TestGenerateTURNCredentialsAt(t *testing.T) {
	now := time.Date(2026, 4, 4, 12, 0, 0, 0, time.UTC)

	username, credential, err := GenerateTURNCredentialsAt("turn-secret", "u1001", 24*time.Hour, now)
	if err != nil {
		t.Fatalf("generate turn credentials failed: %v", err)
	}

	if username != "1775390400:u1001" {
		t.Fatalf("unexpected turn username: %q", username)
	}
	if credential != "AY7zb5jJqd7/cV9JBSFx7Pq+WGU=" {
		t.Fatalf("unexpected turn credential: %q", credential)
	}
}

func TestGenerateTURNCredentialsAtValidation(t *testing.T) {
	now := time.Now().UTC()

	if _, _, err := GenerateTURNCredentialsAt("", "u1001", time.Hour, now); err == nil {
		t.Fatalf("expected secret validation failure")
	}
	if _, _, err := GenerateTURNCredentialsAt("secret", "", time.Hour, now); err == nil {
		t.Fatalf("expected user id validation failure")
	}
	if _, _, err := GenerateTURNCredentialsAt("secret", "u1001", 0, now); err == nil {
		t.Fatalf("expected ttl validation failure")
	}
}
