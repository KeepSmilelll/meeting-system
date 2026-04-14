package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"strings"
	"testing"
	"time"
)

func TestTokenGenerateVerify(t *testing.T) {
	m := NewTokenManager("secret", time.Minute)
	token, err := m.Generate("u1001")
	if err != nil {
		t.Fatalf("Generate failed: %v", err)
	}

	if gotParts := len(strings.Split(token, ".")); gotParts != 3 {
		t.Fatalf("expected JWT token with 3 segments, got %d", gotParts)
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

func TestTokenVerifyLegacyCompatibility(t *testing.T) {
	m := NewTokenManager("secret", time.Minute)

	exp := time.Now().Add(time.Minute).UnixMilli()
	payload := fmt.Sprintf("%s|%d", "u1002", exp)
	payloadSegment := base64.RawURLEncoding.EncodeToString([]byte(payload))

	mac := hmac.New(sha256.New, []byte("secret"))
	_, _ = mac.Write([]byte(payload))
	signatureSegment := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))

	legacyToken := payloadSegment + "." + signatureSegment

	uid, err := m.Verify(legacyToken)
	if err != nil {
		t.Fatalf("Verify legacy token failed: %v", err)
	}
	if uid != "u1002" {
		t.Fatalf("unexpected user id: %s", uid)
	}
}
