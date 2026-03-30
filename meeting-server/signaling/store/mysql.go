package store

import (
	"meeting-server/signaling/config"
	"meeting-server/signaling/model"

	"gorm.io/driver/mysql"
	"gorm.io/gorm"
)

func NewMySQL(cfg config.Config) (*gorm.DB, error) {
	if cfg.MySQLDSN == "" {
		return nil, nil
	}

	db, err := gorm.Open(mysql.Open(cfg.MySQLDSN), &gorm.Config{})
	if err != nil {
		return nil, err
	}

	sqlDB, err := db.DB()
	if err != nil {
		return nil, err
	}

	sqlDB.SetMaxOpenConns(cfg.MySQLMaxOpenConns)
	sqlDB.SetMaxIdleConns(cfg.MySQLMaxIdleConns)
	sqlDB.SetConnMaxLifetime(cfg.MySQLConnMaxLife)

	return db, nil
}

func AutoMigrate(db *gorm.DB) error {
	if db == nil {
		return nil
	}

	return db.AutoMigrate(
		&model.User{},
		&model.Meeting{},
		&model.Participant{},
		&model.Message{},
		&model.Friendship{},
	)
}

func PingMySQL(db *gorm.DB) error {
	if db == nil {
		return nil
	}

	sqlDB, err := db.DB()
	if err != nil {
		return err
	}

	return sqlDB.Ping()
}
