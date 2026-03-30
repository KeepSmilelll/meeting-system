package model

import "time"

type User struct {
	ID           uint64    `gorm:"primaryKey;autoIncrement"`
	Username     string    `gorm:"size:64;uniqueIndex;not null"`
	DisplayName  string    `gorm:"size:64;not null;default:''"`
	AvatarURL    string    `gorm:"size:255;default:''"`
	PasswordHash string    `gorm:"size:255;not null"`
	Status       int       `gorm:"not null;default:0"`
	CreatedAt    time.Time `gorm:"autoCreateTime"`
	UpdatedAt    time.Time `gorm:"autoUpdateTime"`
}

func (User) TableName() string { return "users" }
