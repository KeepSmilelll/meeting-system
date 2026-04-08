package config

import (
	"testing"
	"time"
)

func TestLoadDefaultsIncludesRecoveryTunables(t *testing.T) {
	t.Setenv("SIGNALING_SFU_RECOVERY_LOCK_TTL", "")
	t.Setenv("SIGNALING_SFU_RECOVERY_FOLLOWUP_DELAY", "")

	cfg := Load()
	if cfg.SFURecoveryLockTTL != 8*time.Second {
		t.Fatalf("unexpected default recovery lock ttl: got %v want %v", cfg.SFURecoveryLockTTL, 8*time.Second)
	}
	if cfg.SFURecoveryFollowup != 250*time.Millisecond {
		t.Fatalf("unexpected default recovery followup delay: got %v want %v", cfg.SFURecoveryFollowup, 250*time.Millisecond)
	}
}

func TestLoadRecoveryTunablesFromEnv(t *testing.T) {
	t.Setenv("SIGNALING_SFU_RECOVERY_LOCK_TTL", "15s")
	t.Setenv("SIGNALING_SFU_RECOVERY_FOLLOWUP_DELAY", "750ms")

	cfg := Load()
	if cfg.SFURecoveryLockTTL != 15*time.Second {
		t.Fatalf("unexpected configured recovery lock ttl: got %v want %v", cfg.SFURecoveryLockTTL, 15*time.Second)
	}
	if cfg.SFURecoveryFollowup != 750*time.Millisecond {
		t.Fatalf("unexpected configured recovery followup delay: got %v want %v", cfg.SFURecoveryFollowup, 750*time.Millisecond)
	}
}

func TestLoadRecoveryTunablesFallbackOnInvalidEnv(t *testing.T) {
	t.Setenv("SIGNALING_SFU_RECOVERY_LOCK_TTL", "not-a-duration")
	t.Setenv("SIGNALING_SFU_RECOVERY_FOLLOWUP_DELAY", "invalid")

	cfg := Load()
	if cfg.SFURecoveryLockTTL != 8*time.Second {
		t.Fatalf("unexpected fallback recovery lock ttl: got %v want %v", cfg.SFURecoveryLockTTL, 8*time.Second)
	}
	if cfg.SFURecoveryFollowup != 250*time.Millisecond {
		t.Fatalf("unexpected fallback recovery followup delay: got %v want %v", cfg.SFURecoveryFollowup, 250*time.Millisecond)
	}
}

func TestLoadAdminListenAddrFromEnv(t *testing.T) {
	t.Setenv("SIGNALING_ADMIN_LISTEN_ADDR", "127.0.0.1:19191")

	cfg := Load()
	if cfg.AdminListenAddr != "127.0.0.1:19191" {
		t.Fatalf("unexpected admin listen addr: got %q want %q", cfg.AdminListenAddr, "127.0.0.1:19191")
	}
}

func TestLoadDefaultsIncludesRouteStatusDedupWindow(t *testing.T) {
	t.Setenv("SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW", "")

	cfg := Load()
	if cfg.SFURouteStatusDedup != 3*time.Second {
		t.Fatalf("unexpected default route status dedup window: got %v want %v", cfg.SFURouteStatusDedup, 3*time.Second)
	}
}

func TestLoadRouteStatusDedupWindowFromEnv(t *testing.T) {
	t.Setenv("SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW", "900ms")

	cfg := Load()
	if cfg.SFURouteStatusDedup != 900*time.Millisecond {
		t.Fatalf("unexpected configured route status dedup window: got %v want %v", cfg.SFURouteStatusDedup, 900*time.Millisecond)
	}
}

func TestLoadRouteStatusDedupWindowFallbackOnInvalidEnv(t *testing.T) {
	t.Setenv("SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW", "not-a-duration")

	cfg := Load()
	if cfg.SFURouteStatusDedup != 3*time.Second {
		t.Fatalf("unexpected fallback route status dedup window: got %v want %v", cfg.SFURouteStatusDedup, 3*time.Second)
	}
}
