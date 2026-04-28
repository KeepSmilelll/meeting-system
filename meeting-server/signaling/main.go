package main

import (
	"context"
	"log"
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
	log.Printf("signaling config listen=%s node=%s auth_timeout=%s idle_timeout=%s redis_enabled=%t redis_addr=%s mysql_enabled=%t sfu_rpc=%s default_sfu=%s turn_servers=%d",
		cfg.ListenAddr,
		cfg.NodeID,
		cfg.AuthTimeout,
		cfg.IdleTimeout,
		cfg.EnableRedis,
		cfg.RedisAddr,
		cfg.MySQLDSN != "",
		cfg.SFURPCAddr,
		cfg.DefaultSFUAddress,
		len(cfg.TURNServers),
	)

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
		memoryStore.SetUserRepo(store.NewUserRepo(mysqlDB))
		if err := memoryStore.SeedDefaultUsersToRepo(context.Background()); err != nil {
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
	messageRepo := store.NewInMemoryMessageRepo()
	if mysqlDB != nil {
		meetingStore = store.NewMeetingLifecycleStoreWithParticipantRepo(
			memoryStore,
			store.NewMeetingRepo(mysqlDB),
			store.NewParticipantRepo(mysqlDB),
		)
		messageRepo = store.NewMessageRepo(mysqlDB)
	}

	router := handler.NewRouterWithMessageRepo(cfg, sessions, memoryStore, meetingStore, roomStateStore, sessionStore, tokenManager, limiter, sfuClient, nodeEventBus, messageRepo)
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
