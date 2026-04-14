package pkg

import (
	"fmt"
	"sync"
	"time"
)

const (
	snowflakeNodeBits     = 10
	snowflakeSequenceBits = 12
	snowflakeNodeMax      = (1 << snowflakeNodeBits) - 1
	snowflakeSequenceMask = (1 << snowflakeSequenceBits) - 1
	snowflakeEpochMs      = int64(1704067200000) // 2024-01-01T00:00:00Z
)

type Snowflake struct {
	mu       sync.Mutex
	nodeID   uint16
	lastMs   int64
	sequence uint16
}

func NewSnowflake(nodeID uint16) (*Snowflake, error) {
	if nodeID > snowflakeNodeMax {
		return nil, fmt.Errorf("snowflake: node id %d out of range (max %d)", nodeID, snowflakeNodeMax)
	}

	return &Snowflake{nodeID: nodeID}, nil
}

func (g *Snowflake) Next() uint64 {
	g.mu.Lock()
	defer g.mu.Unlock()

	nowMs := time.Now().UnixMilli()
	if nowMs < g.lastMs {
		nowMs = g.lastMs
	}

	if nowMs == g.lastMs {
		g.sequence = (g.sequence + 1) & snowflakeSequenceMask
		if g.sequence == 0 {
			for nowMs <= g.lastMs {
				time.Sleep(time.Millisecond)
				nowMs = time.Now().UnixMilli()
			}
		}
	} else {
		g.sequence = 0
	}

	g.lastMs = nowMs
	timestampPart := uint64(nowMs-snowflakeEpochMs) << (snowflakeNodeBits + snowflakeSequenceBits)
	nodePart := uint64(g.nodeID) << snowflakeSequenceBits
	sequencePart := uint64(g.sequence)
	return timestampPart | nodePart | sequencePart
}
