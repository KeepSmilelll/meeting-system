package main

import (
	"context"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/handler"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	cfg := config.Load()

	sessions := server.NewSessionManager()
	memoryStore := store.NewMemoryStore()

	mysqlDB, err := store.NewMySQL(cfg)
	if err != nil {
		panic(err)
	}
	if mysqlDB != nil {
		if sqlDB, err := mysqlDB.DB(); err == nil {
			defer sqlDB.Close()
		}
		if err := store.AutoMigrate(mysqlDB); err != nil {
			panic(err)
		}
	}

	redisClient := store.NewRedisClient(cfg)
	if redisClient != nil {
		defer redisClient.Close()
	}

	roomStateStore := store.NewRedisRoomStoreWithClient(redisClient)
	tokenManager := auth.NewTokenManager(cfg.JWTSecret, cfg.TokenTTL)
	limiter := auth.NewRateLimiter(redisClient, 5, 10*time.Minute)
	sfuClient := signalingSfu.NewClient(cfg.SFURPCAddr, signalingSfu.WithTimeout(cfg.SFURPCTimeout))
	meetingMirror := handler.NewSFUMeetingMirror(sfuClient)

	var meetingStore store.MeetingLifecycleStore = memoryStore
	if mysqlDB != nil {
		meetingStore = store.NewMeetingLifecycleStore(memoryStore, store.NewMeetingRepo(mysqlDB))
	}

	router := handler.NewRouter(cfg, sessions, memoryStore, meetingStore, roomStateStore, tokenManager, limiter, meetingMirror)
	tcpServer := server.NewTCPServer(cfg, sessions, router)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	if err := tcpServer.Run(ctx); err != nil {
		panic(err)
	}
}
