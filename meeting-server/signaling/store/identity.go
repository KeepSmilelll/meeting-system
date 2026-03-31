package store

import (
	"fmt"
	"strconv"
	"strings"
)

// External IDs stay in the existing signaling format:
// users are "u12345" and meeting numbers are opaque strings.
// The repository layer only stores the numeric suffix for users.
func userIDToUint64(userID string) (uint64, error) {
	if userID == "" {
		return 0, fmt.Errorf("user id is required")
	}

	if strings.HasPrefix(userID, "u") {
		userID = strings.TrimPrefix(userID, "u")
	}
	if userID == "" {
		return 0, fmt.Errorf("user id is required")
	}

	return strconv.ParseUint(userID, 10, 64)
}

func userIDFromUint64(userID uint64) string {
	return fmt.Sprintf("u%d", userID)
}
