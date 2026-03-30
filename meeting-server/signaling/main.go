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
)

func main() {
    cfg := config.Load()

    sessions := server.NewSessionManager()
    memoryStore := store.NewMemoryStore()
    tokenManager := auth.NewTokenManager(cfg.JWTSecret, cfg.TokenTTL)

    router := handler.NewRouter(cfg, sessions, memoryStore, tokenManager)
    tcpServer := server.NewTCPServer(cfg, sessions, router)

    ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
    defer stop()

    if err := tcpServer.Run(ctx); err != nil {
        panic(err)
    }
}

