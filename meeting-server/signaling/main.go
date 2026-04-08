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
	sessionStore := store.NewRedisSessionStoreWithClient(cfg, redisClient)
	roomEventBus := store.NewRedisRoomEventBusWithClient(cfg, redisClient)
	nodeEventBus := store.NewRedisNodeEventBusWithClient(cfg, redisClient)
	tokenManager := auth.NewTokenManager(cfg.JWTSecret, cfg.TokenTTL)
	limiter := auth.NewRateLimiter(redisClient, 5, 10*time.Minute)
	sfuClient := signalingSfu.NewRoutedClient(cfg, roomStateStore, signalingSfu.WithTimeout(cfg.SFURPCTimeout))

	var meetingStore store.MeetingLifecycleStore = memoryStore
	if mysqlDB != nil {
		meetingStore = store.NewMeetingLifecycleStoreWithParticipantRepo(
			memoryStore,
			store.NewMeetingRepo(mysqlDB),
			store.NewParticipantRepo(mysqlDB),
		)
	}

	router := handler.NewRouter(cfg, sessions, memoryStore, meetingStore, roomStateStore, sessionStore, tokenManager, limiter, sfuClient, nodeEventBus)
	tcpServer := server.NewTCPServer(cfg, sessions, router)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer func() {
		stop()
		_ = nodeEventBus.Close()
		_ = roomEventBus.Close()
	}()

	sessions.SetMeetingFramePublisher(roomEventBus)
	if err := roomEventBus.Start(ctx, sessions.DeliverMeetingFrame); err != nil {
		panic(err)
	}
	if err := nodeEventBus.Start(ctx, router.OnUserNodeEvent); err != nil {
		panic(err)
	}

	if err := signalingSfu.NewReportServer(cfg, roomStateStore).Start(ctx); err != nil {
		panic(err)
	}
	if err := signalingSfu.NewAdminServer(cfg, roomStateStore).Start(ctx); err != nil {
		panic(err)
	}
	signalingSfu.NewNodeStatusMonitor(cfg, roomStateStore, signalingSfu.WithTimeout(cfg.SFURPCTimeout)).Start(ctx)

	if err := tcpServer.Run(ctx); err != nil {
		panic(err)
	}
}
