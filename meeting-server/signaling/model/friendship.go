package model

import "time"

type Friendship struct {
	ID         uint64    `gorm:"primaryKey;autoIncrement"`
	UserID     uint64    `gorm:"index;not null"`
	FriendID   uint64    `gorm:"index;not null"`
	RemarkName string    `gorm:"size:64;default:''"`
	Status     int       `gorm:"not null;default:0"`
	CreatedAt  time.Time `gorm:"autoCreateTime"`
}

func (Friendship) TableName() string { return "friendships" }
