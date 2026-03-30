package model

import "time"

type Meeting struct {
	ID              uint64    `gorm:"primaryKey;autoIncrement"`
	MeetingNo       string    `gorm:"size:16;uniqueIndex;not null"`
	Title           string    `gorm:"size:128;not null"`
	HostUserID      uint64    `gorm:"index;not null"`
	PasswordHash    string    `gorm:"size:255;default:''"`
	Status          int       `gorm:"not null;default:0"`
	MaxParticipants int       `gorm:"not null;default:16"`
	CreatedAt       time.Time `gorm:"autoCreateTime"`
	EndedAt         *time.Time
}

func (Meeting) TableName() string { return "meetings" }
