package auth

import (
	"context"
	"fmt"
	"time"

	"github.com/redis/go-redis/v9"
)

type RateLimiter struct {
	client *redis.Client
	limit  int64
	window time.Duration
	prefix string
}

func NewRateLimiter(client *redis.Client, limit int64, window time.Duration) *RateLimiter {
	return &RateLimiter{
		client: client,
		limit:  limit,
		window: window,
		prefix: "rate_limit:",
	}
}

func (r *RateLimiter) Allow(ctx context.Context, key string) (bool, int64, error) {
	if r == nil || r.client == nil || r.limit <= 0 || r.window <= 0 {
		return true, 0, nil
	}

	redisKey := r.prefix + key
	script := redis.NewScript(`
local v = redis.call('INCR', KEYS[1])
if v == 1 then
  redis.call('PEXPIRE', KEYS[1], ARGV[1])
end
return v
`)

	result, err := script.Run(ctx, r.client, []string{redisKey}, r.window.Milliseconds()).Int64()
	if err != nil {
		return false, 0, fmt.Errorf("rate limit increment failed: %w", err)
	}

	if result > r.limit {
		return false, result, nil
	}

	return true, result, nil
}
