package auth

import (
    "crypto/hmac"
    "crypto/sha256"
    "encoding/base64"
    "errors"
    "fmt"
    "strconv"
    "strings"
    "time"
)

var ErrTokenInvalid = errors.New("invalid token")

type TokenManager struct {
    secret []byte
    ttl    time.Duration
}

func NewTokenManager(secret string, ttl time.Duration) *TokenManager {
    return &TokenManager{secret: []byte(secret), ttl: ttl}
}

func (m *TokenManager) Generate(userID string) (string, error) {
    exp := time.Now().Add(m.ttl).Unix()
    payload := fmt.Sprintf("%s|%d", userID, exp)
    sig := m.sign(payload)
    token := base64.RawURLEncoding.EncodeToString([]byte(payload)) + "." + base64.RawURLEncoding.EncodeToString(sig)
    return token, nil
}

func (m *TokenManager) Verify(token string) (string, error) {
    parts := strings.Split(token, ".")
    if len(parts) != 2 {
        return "", ErrTokenInvalid
    }

    payloadBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
    if err != nil {
        return "", ErrTokenInvalid
    }
    sigBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
    if err != nil {
        return "", ErrTokenInvalid
    }

    if !hmac.Equal(sigBytes, m.sign(string(payloadBytes))) {
        return "", ErrTokenInvalid
    }

    payload := strings.Split(string(payloadBytes), "|")
    if len(payload) != 2 {
        return "", ErrTokenInvalid
    }

    exp, err := strconv.ParseInt(payload[1], 10, 64)
    if err != nil {
        return "", ErrTokenInvalid
    }
    if time.Now().Unix() > exp {
        return "", ErrTokenInvalid
    }

    return payload[0], nil
}

func (m *TokenManager) sign(payload string) []byte {
    mac := hmac.New(sha256.New, m.secret)
    _, _ = mac.Write([]byte(payload))
    return mac.Sum(nil)
}

