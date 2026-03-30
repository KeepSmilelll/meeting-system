package main

import (
	"context"
	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/handler"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"
	"os/signal"
	"syscall"
	"time"
)

func main() {
	cfg := config.Load()

	sessions := server.NewSessionManager()
	memoryStore := store.NewMemoryStore()

	redisClient := store.NewRedisClient(cfg)
	if redisClient != nil {
		defer redisClient.Close()
	}

	roomStateStore := store.NewRedisRoomStoreWithClient(redisClient)
	tokenManager := auth.NewTokenManager(cfg.JWTSecret, cfg.TokenTTL)
	limiter := auth.NewRateLimiter(redisClient, 5, 10*time.Minute)

	router := handler.NewRouter(cfg, sessions, memoryStore, roomStateStore, tokenManager, limiter)
	tcpServer := server.NewTCPServer(cfg, sessions, router)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	if err := tcpServer.Run(ctx); err != nil {
		panic(err)
	}
}
