package config

import (
    "os"
    "strconv"
    "time"
)

type Config struct {
    ListenAddr        string
    ReadTimeout       time.Duration
    WriteTimeout      time.Duration
    IdleTimeout       time.Duration
    AuthTimeout       time.Duration
    MaxPayloadBytes   uint32
    WriteQueueSize    int
    MaxMessageLen     int
    JWTSecret         string
    TokenTTL          time.Duration
    DefaultSFUAddress string
}

func Load() Config {
    return Config{
        ListenAddr:        getEnv("SIGNALING_LISTEN_ADDR", ":8443"),
        ReadTimeout:       getDuration("SIGNALING_READ_TIMEOUT", 90*time.Second),
        WriteTimeout:      getDuration("SIGNALING_WRITE_TIMEOUT", 10*time.Second),
        IdleTimeout:       getDuration("SIGNALING_IDLE_TIMEOUT", 90*time.Second),
        AuthTimeout:       getDuration("SIGNALING_AUTH_TIMEOUT", 10*time.Second),
        MaxPayloadBytes:   uint32(getInt("SIGNALING_MAX_PAYLOAD", 1<<20)),
        WriteQueueSize:    getInt("SIGNALING_WRITE_QUEUE", 256),
        MaxMessageLen:     getInt("SIGNALING_CHAT_MAX_LEN", 5000),
        JWTSecret:         getEnv("SIGNALING_JWT_SECRET", "dev-secret-change-me"),
        TokenTTL:          getDuration("SIGNALING_TOKEN_TTL", 7*24*time.Hour),
        DefaultSFUAddress: getEnv("SIGNALING_DEFAULT_SFU", "127.0.0.1:10000"),
    }
}

func getEnv(key, fallback string) string {
    if val, ok := os.LookupEnv(key); ok && val != "" {
        return val
    }
    return fallback
}

func getInt(key string, fallback int) int {
    if val, ok := os.LookupEnv(key); ok && val != "" {
        parsed, err := strconv.Atoi(val)
        if err == nil {
            return parsed
        }
    }
    return fallback
}

func getDuration(key string, fallback time.Duration) time.Duration {
    if val, ok := os.LookupEnv(key); ok && val != "" {
        parsed, err := time.ParseDuration(val)
        if err == nil {
            return parsed
        }
    }
    return fallback
}

