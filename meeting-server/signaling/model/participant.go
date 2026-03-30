package model

import "time"

type Participant struct {
	ID         uint64    `gorm:"primaryKey;autoIncrement"`
	MeetingID  uint64    `gorm:"index;not null"`
	UserID     uint64    `gorm:"index;not null"`
	Role       int       `gorm:"not null;default:0"`
	MediaState int       `gorm:"not null;default:0"`
	JoinedAt   time.Time `gorm:"autoCreateTime"`
	LeftAt     *time.Time
}

func (Participant) TableName() string { return "meeting_participants" }
