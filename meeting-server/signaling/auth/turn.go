package auth

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"fmt"
	"time"
)

func GenerateTURNCredentials(secret, userID string, ttl time.Duration) (string, string, error) {
	return GenerateTURNCredentialsAt(secret, userID, ttl, time.Now().UTC())
}

func GenerateTURNCredentialsAt(secret, userID string, ttl time.Duration, now time.Time) (string, string, error) {
	if secret == "" {
		return "", "", fmt.Errorf("turn credentials: secret is required")
	}
	if userID == "" {
		return "", "", fmt.Errorf("turn credentials: user id is required")
	}
	if ttl <= 0 {
		return "", "", fmt.Errorf("turn credentials: ttl must be positive")
	}

	username := fmt.Sprintf("%d:%s", now.Add(ttl).Unix(), userID)
	mac := hmac.New(sha1.New, []byte(secret))
	_, _ = mac.Write([]byte(username))
	credential := base64.StdEncoding.EncodeToString(mac.Sum(nil))
	return username, credential, nil
}
