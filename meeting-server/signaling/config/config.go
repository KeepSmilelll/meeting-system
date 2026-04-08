package config

import (
	"os"
	"strconv"
	"strings"
	"time"
)

type Config struct {
	ListenAddr            string
	NodeID                string
	ReadTimeout           time.Duration
	WriteTimeout          time.Duration
	IdleTimeout           time.Duration
	AuthTimeout           time.Duration
	MaxPayloadBytes       uint32
	WriteQueueSize        int
	MaxMessageLen         int
	JWTSecret             string
	TokenTTL              time.Duration
	DefaultSFUAddress     string
	DefaultSFUNodeID      string
	SFURPCAddr            string
	SFUNodes              []SFUNode
	SFURouteRPCMap        map[string]string
	SFURPCTimeout         time.Duration
	SFUNodeQuarantine     time.Duration
	SFURecoveryLockTTL    time.Duration
	SFURecoveryFollowup   time.Duration
	SFURouteStatusDedup   time.Duration
	SFUStatusPollInterval time.Duration
	SFUStatusTTL          time.Duration
	SFUReportListenAddr   string
	AdminListenAddr       string
	TURNSecret            string
	TURNRealm             string
	TURNCredTTL           time.Duration
	TURNServers           []string

	EnableRedis   bool
	RedisAddr     string
	RedisPassword string
	RedisDB       int

	MySQLDSN          string
	MySQLMaxOpenConns int
	MySQLMaxIdleConns int
	MySQLConnMaxLife  time.Duration
}

type SFUNode struct {
	NodeID       string
	MediaAddress string
	RPCAddress   string
	MaxMeetings  int
}

func Load() Config {
	return Config{
		ListenAddr:            getEnv("SIGNALING_LISTEN_ADDR", ":8443"),
		NodeID:                getEnv("SIGNALING_NODE_ID", "sig-node-01"),
		ReadTimeout:           getDuration("SIGNALING_READ_TIMEOUT", 90*time.Second),
		WriteTimeout:          getDuration("SIGNALING_WRITE_TIMEOUT", 10*time.Second),
		IdleTimeout:           getDuration("SIGNALING_IDLE_TIMEOUT", 90*time.Second),
		AuthTimeout:           getDuration("SIGNALING_AUTH_TIMEOUT", 10*time.Second),
		MaxPayloadBytes:       uint32(getInt("SIGNALING_MAX_PAYLOAD", 1<<20)),
		WriteQueueSize:        getInt("SIGNALING_WRITE_QUEUE", 256),
		MaxMessageLen:         getInt("SIGNALING_CHAT_MAX_LEN", 5000),
		JWTSecret:             getEnv("SIGNALING_JWT_SECRET", "dev-secret-change-me"),
		TokenTTL:              getDuration("SIGNALING_TOKEN_TTL", 7*24*time.Hour),
		DefaultSFUAddress:     getEnv("SIGNALING_DEFAULT_SFU", "127.0.0.1:10000"),
		DefaultSFUNodeID:      getEnv("SIGNALING_DEFAULT_SFU_NODE_ID", "default-sfu"),
		SFURPCAddr:            getEnv("SIGNALING_SFU_RPC_ADDR", ""),
		SFUNodes:              getSFUNodes("SIGNALING_SFU_NODES"),
		SFURouteRPCMap:        getAssignments("SIGNALING_SFU_ROUTE_RPC_MAP"),
		SFURPCTimeout:         getDuration("SIGNALING_SFU_RPC_TIMEOUT", 5*time.Second),
		SFUNodeQuarantine:     getDuration("SIGNALING_SFU_NODE_QUARANTINE", 30*time.Second),
		SFURecoveryLockTTL:    getDuration("SIGNALING_SFU_RECOVERY_LOCK_TTL", 8*time.Second),
		SFURecoveryFollowup:   getDuration("SIGNALING_SFU_RECOVERY_FOLLOWUP_DELAY", 250*time.Millisecond),
		SFURouteStatusDedup:   getDuration("SIGNALING_SFU_ROUTE_STATUS_DEDUP_WINDOW", 3*time.Second),
		SFUStatusPollInterval: getDuration("SIGNALING_SFU_STATUS_POLL_INTERVAL", 5*time.Second),
		SFUStatusTTL:          getDuration("SIGNALING_SFU_STATUS_TTL", 20*time.Second),
		SFUReportListenAddr:   getEnv("SIGNALING_SFU_REPORT_ADDR", ""),
		AdminListenAddr:       getEnv("SIGNALING_ADMIN_LISTEN_ADDR", ""),
		TURNSecret:            getEnv("SIGNALING_TURN_SECRET", ""),
		TURNRealm:             getEnv("SIGNALING_TURN_REALM", ""),
		TURNCredTTL:           getDuration("SIGNALING_TURN_CRED_TTL", 24*time.Hour),
		TURNServers:           getCSV("SIGNALING_TURN_SERVERS"),

		EnableRedis:   getBool("SIGNALING_ENABLE_REDIS", false),
		RedisAddr:     getEnv("SIGNALING_REDIS_ADDR", "127.0.0.1:6379"),
		RedisPassword: getEnv("SIGNALING_REDIS_PASSWORD", ""),
		RedisDB:       getInt("SIGNALING_REDIS_DB", 0),

		MySQLDSN:          getEnv("SIGNALING_MYSQL_DSN", ""),
		MySQLMaxOpenConns: getInt("SIGNALING_MYSQL_MAX_OPEN", 20),
		MySQLMaxIdleConns: getInt("SIGNALING_MYSQL_MAX_IDLE", 10),
		MySQLConnMaxLife:  getDuration("SIGNALING_MYSQL_CONN_MAX_LIFE", time.Hour),
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

func getBool(key string, fallback bool) bool {
	if val, ok := os.LookupEnv(key); ok && val != "" {
		parsed, err := strconv.ParseBool(val)
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

func getCSV(key string) []string {
	val, ok := os.LookupEnv(key)
	if !ok || val == "" {
		return nil
	}

	parts := make([]string, 0)
	for _, raw := range strings.Split(val, ",") {
		trimmed := strings.TrimSpace(raw)
		if trimmed == "" {
			continue
		}
		parts = append(parts, trimmed)
	}
	return parts
}

func getAssignments(key string) map[string]string {
	val, ok := os.LookupEnv(key)
	if !ok || val == "" {
		return nil
	}

	assignments := make(map[string]string)
	for _, raw := range strings.Split(val, ",") {
		pair := strings.SplitN(strings.TrimSpace(raw), "=", 2)
		if len(pair) != 2 {
			continue
		}

		k := strings.TrimSpace(pair[0])
		v := strings.TrimSpace(pair[1])
		if k == "" || v == "" {
			continue
		}
		assignments[k] = v
	}
	if len(assignments) == 0 {
		return nil
	}
	return assignments
}

func getSFUNodes(key string) []SFUNode {
	val, ok := os.LookupEnv(key)
	if !ok || val == "" {
		return nil
	}

	nodes := make([]SFUNode, 0)
	for _, raw := range strings.Split(val, ",") {
		parts := strings.Split(strings.TrimSpace(raw), "|")
		if len(parts) < 3 || len(parts) > 4 {
			continue
		}

		nodeID := strings.TrimSpace(parts[0])
		mediaAddr := strings.TrimSpace(parts[1])
		rpcAddr := strings.TrimSpace(parts[2])
		if nodeID == "" || mediaAddr == "" || rpcAddr == "" {
			continue
		}

		maxMeetings := 0
		if len(parts) == 4 {
			parsed, err := strconv.Atoi(strings.TrimSpace(parts[3]))
			if err == nil && parsed > 0 {
				maxMeetings = parsed
			}
		}

		nodes = append(nodes, SFUNode{
			NodeID:       nodeID,
			MediaAddress: mediaAddr,
			RPCAddress:   rpcAddr,
			MaxMeetings:  maxMeetings,
		})
	}
	if len(nodes) == 0 {
		return nil
	}
	return nodes
}

func (c Config) EffectiveSFUNodes() []SFUNode {
	if len(c.SFUNodes) > 0 {
		nodes := make([]SFUNode, 0, len(c.SFUNodes))
		for _, node := range c.SFUNodes {
			if node.NodeID == "" || node.MediaAddress == "" {
				continue
			}

			current := node
			if current.RPCAddress == "" {
				if mapped, ok := c.SFURouteRPCMap[current.MediaAddress]; ok {
					current.RPCAddress = mapped
				} else if current.MediaAddress == c.DefaultSFUAddress {
					current.RPCAddress = c.SFURPCAddr
				}
			}
			nodes = append(nodes, current)
		}
		if len(nodes) > 0 {
			return nodes
		}
	}

	return []SFUNode{{
		NodeID:       c.DefaultSFUNodeID,
		MediaAddress: c.DefaultSFUAddress,
		RPCAddress:   c.SFURPCAddr,
		MaxMeetings:  0,
	}}
}
