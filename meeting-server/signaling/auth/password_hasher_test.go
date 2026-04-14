package auth

import (
	"errors"
	"testing"
)

func TestPasswordHasher_HashAndVerify(t *testing.T) {
	hasher := NewPasswordHasher()
	hash, err := hasher.HashPassword("demo-password")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}
	if !IsArgon2idHash(hash) {
		t.Fatalf("expected argon2id hash, got %q", hash)
	}

	if err := hasher.VerifyPassword(hash, "demo-password"); err != nil {
		t.Fatalf("VerifyPassword failed: %v", err)
	}
}

func TestPasswordHasher_VerifyRejectsInvalidOrWrongPassword(t *testing.T) {
	hasher := NewPasswordHasher()
	hash, err := hasher.HashPassword("secret")
	if err != nil {
		t.Fatalf("HashPassword failed: %v", err)
	}

	if err := hasher.VerifyPassword(hash, "wrong"); !errors.Is(err, ErrPasswordMismatch) {
		t.Fatalf("expected ErrPasswordMismatch, got %v", err)
	}

	if err := hasher.VerifyPassword("not-argon", "secret"); !errors.Is(err, ErrPasswordHashInvalid) {
		t.Fatalf("expected ErrPasswordHashInvalid, got %v", err)
	}
}
