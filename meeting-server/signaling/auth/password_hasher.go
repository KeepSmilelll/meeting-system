package auth

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strings"

	"golang.org/x/crypto/argon2"
)

const (
	argon2Version = 19
	argon2Prefix  = "$argon2id$"
)

var (
	ErrPasswordHashInvalid = errors.New("invalid password hash")
	ErrPasswordMismatch    = errors.New("password mismatch")
)

type PasswordHasher struct {
	memoryKB   uint32
	iterations uint32
	parallel   uint8
	saltLen    uint32
	keyLen     uint32
}

func NewPasswordHasher() *PasswordHasher {
	return &PasswordHasher{
		memoryKB:   64 * 1024,
		iterations: 3,
		parallel:   2,
		saltLen:    16,
		keyLen:     32,
	}
}

func IsArgon2idHash(encoded string) bool {
	return strings.HasPrefix(encoded, argon2Prefix)
}

func (h *PasswordHasher) HashPassword(password string) (string, error) {
	if h == nil || password == "" {
		return "", ErrPasswordHashInvalid
	}

	salt := make([]byte, h.saltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("read password salt: %w", err)
	}

	derived := argon2.IDKey([]byte(password), salt, h.iterations, h.memoryKB, h.parallel, h.keyLen)
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2Version,
		h.memoryKB,
		h.iterations,
		h.parallel,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(derived),
	), nil
}

func (h *PasswordHasher) VerifyPassword(encodedHash, password string) error {
	if h == nil || encodedHash == "" || password == "" {
		return ErrPasswordHashInvalid
	}

	cfg, salt, expected, err := parseArgon2idHash(encodedHash)
	if err != nil {
		return err
	}

	actual := argon2.IDKey([]byte(password), salt, cfg.iterations, cfg.memoryKB, cfg.parallel, uint32(len(expected)))
	if subtle.ConstantTimeCompare(actual, expected) != 1 {
		return ErrPasswordMismatch
	}
	return nil
}

func parseArgon2idHash(encoded string) (*PasswordHasher, []byte, []byte, error) {
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[1] != "argon2id" {
		return nil, nil, nil, ErrPasswordHashInvalid
	}
	if parts[2] != fmt.Sprintf("v=%d", argon2Version) {
		return nil, nil, nil, ErrPasswordHashInvalid
	}

	var cfg PasswordHasher
	if _, err := fmt.Sscanf(parts[3], "m=%d,t=%d,p=%d", &cfg.memoryKB, &cfg.iterations, &cfg.parallel); err != nil {
		return nil, nil, nil, ErrPasswordHashInvalid
	}
	if cfg.memoryKB == 0 || cfg.iterations == 0 || cfg.parallel == 0 {
		return nil, nil, nil, ErrPasswordHashInvalid
	}

	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil || len(salt) == 0 {
		return nil, nil, nil, ErrPasswordHashInvalid
	}

	key, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(key) == 0 {
		return nil, nil, nil, ErrPasswordHashInvalid
	}

	cfg.saltLen = uint32(len(salt))
	cfg.keyLen = uint32(len(key))

	return &cfg, salt, key, nil
}
