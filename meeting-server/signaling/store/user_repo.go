package store

import (
	"context"
	"meeting-server/signaling/model"

	"gorm.io/gorm"
)

type UserRepo struct {
	db *gorm.DB
}

func NewUserRepo(db *gorm.DB) *UserRepo {
	return &UserRepo{db: db}
}

func (r *UserRepo) FindByUsername(ctx context.Context, username string) (*model.User, error) {
	if r == nil || r.db == nil {
		return nil, gorm.ErrInvalidDB
	}

	var user model.User
	err := r.db.WithContext(ctx).Where("username = ?", username).First(&user).Error
	if err != nil {
		return nil, err
	}

	return &user, nil
}

func (r *UserRepo) Create(ctx context.Context, user *model.User) error {
	if r == nil || r.db == nil {
		return gorm.ErrInvalidDB
	}
	return r.db.WithContext(ctx).Create(user).Error
}

func (r *UserRepo) UpdateStatus(ctx context.Context, userID uint64, status int) error {
	if r == nil || r.db == nil {
		return gorm.ErrInvalidDB
	}

	return r.db.WithContext(ctx).
		Model(&model.User{}).
		Where("id = ?", userID).
		Update("status", status).Error
}
