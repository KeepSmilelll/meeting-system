package auth

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
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

type jwtHeader struct {
	Alg string `json:"alg"`
	Typ string `json:"typ"`
}

type jwtClaims struct {
	Sub string `json:"sub"`
	Exp int64  `json:"exp"`
	Iat int64  `json:"iat"`
}

func NewTokenManager(secret string, ttl time.Duration) *TokenManager {
	return &TokenManager{secret: []byte(secret), ttl: ttl}
}

func (m *TokenManager) Generate(userID string) (string, error) {
	if userID == "" {
		return "", ErrTokenInvalid
	}

	now := time.Now().UnixMilli()
	claims := jwtClaims{
		Sub: userID,
		Exp: now + m.ttl.Milliseconds(),
		Iat: now,
	}

	headerBytes, err := json.Marshal(jwtHeader{Alg: "HS256", Typ: "JWT"})
	if err != nil {
		return "", fmt.Errorf("marshal jwt header: %w", err)
	}
	claimsBytes, err := json.Marshal(claims)
	if err != nil {
		return "", fmt.Errorf("marshal jwt claims: %w", err)
	}

	encodedHeader := base64.RawURLEncoding.EncodeToString(headerBytes)
	encodedClaims := base64.RawURLEncoding.EncodeToString(claimsBytes)
	unsigned := encodedHeader + "." + encodedClaims

	signature := base64.RawURLEncoding.EncodeToString(m.sign(unsigned))
	return unsigned + "." + signature, nil
}

func (m *TokenManager) Verify(token string) (string, error) {
	parts := strings.Split(token, ".")
	switch len(parts) {
	case 3:
		return m.verifyJWT(parts)
	case 2:
		// Compatibility path for legacy tokens generated before JWT migration.
		return m.verifyLegacy(parts)
	default:
		return "", ErrTokenInvalid
	}
}

func (m *TokenManager) verifyJWT(parts []string) (string, error) {
	unsigned := parts[0] + "." + parts[1]

	signature, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil {
		return "", ErrTokenInvalid
	}
	if !hmac.Equal(signature, m.sign(unsigned)) {
		return "", ErrTokenInvalid
	}

	headerBytes, err := base64.RawURLEncoding.DecodeString(parts[0])
	if err != nil {
		return "", ErrTokenInvalid
	}
	var header jwtHeader
	if err := json.Unmarshal(headerBytes, &header); err != nil {
		return "", ErrTokenInvalid
	}
	if header.Alg != "HS256" || header.Typ != "JWT" {
		return "", ErrTokenInvalid
	}

	claimsBytes, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return "", ErrTokenInvalid
	}
	var claims jwtClaims
	if err := json.Unmarshal(claimsBytes, &claims); err != nil {
		return "", ErrTokenInvalid
	}

	if claims.Sub == "" || claims.Exp <= 0 {
		return "", ErrTokenInvalid
	}
	if time.Now().UnixMilli() > claims.Exp {
		return "", ErrTokenInvalid
	}

	return claims.Sub, nil
}

func (m *TokenManager) verifyLegacy(parts []string) (string, error) {
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
	if time.Now().UnixMilli() > exp {
		return "", ErrTokenInvalid
	}

	return payload[0], nil
}

func (m *TokenManager) sign(payload string) []byte {
	mac := hmac.New(sha256.New, m.secret)
	_, _ = mac.Write([]byte(payload))
	return mac.Sum(nil)
}
