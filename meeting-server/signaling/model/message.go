package model

import "time"

type Message struct {
	ID        uint64 `gorm:"primaryKey;autoIncrement"`
	MeetingID uint64 `gorm:"index;not null"`
	SenderID  uint64 `gorm:"index;not null"`
	Type      int    `gorm:"not null;default:0"`
	Content   string `gorm:"type:text;not null"`
	ReplyToID *uint64
	CreatedAt time.Time `gorm:"autoCreateTime"`
}

func (Message) TableName() string { return "messages" }
