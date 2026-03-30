package server

import (
    "context"
    "errors"
    "meeting-server/signaling/config"
    "meeting-server/signaling/protocol"
    "net"
    "sync/atomic"
)

type MessageHandler interface {
    HandleMessage(session *Session, msgType protocol.SignalType, payload []byte)
}

type SessionCloseHandler interface {
    OnSessionClosed(session *Session)
}

type TCPServer struct {
    cfg      config.Config
    sessions *SessionManager
    handler  MessageHandler

    nextID atomic.Uint64
}

func NewTCPServer(cfg config.Config, sessions *SessionManager, handler MessageHandler) *TCPServer {
    return &TCPServer{cfg: cfg, sessions: sessions, handler: handler}
}

func (s *TCPServer) Run(ctx context.Context) error {
    listener, err := net.Listen("tcp", s.cfg.ListenAddr)
    if err != nil {
        return err
    }
    defer listener.Close()

    done := make(chan struct{})
    go func() {
        select {
        case <-ctx.Done():
            _ = listener.Close()
        case <-done:
        }
    }()

    for {
        conn, err := listener.Accept()
        if err != nil {
            if errors.Is(err, net.ErrClosed) {
                close(done)
                return nil
            }
            continue
        }

        id := s.nextID.Add(1)
        session := NewSession(id, conn, s.cfg)
        s.sessions.Add(session)

        session.Start(func(sess *Session, msgType protocol.SignalType, payload []byte) {
            s.handler.HandleMessage(sess, msgType, payload)
        }, func(sess *Session) {
            if closeHandler, ok := s.handler.(SessionCloseHandler); ok {
                closeHandler.OnSessionClosed(sess)
            }
            s.sessions.Remove(sess)
        })
    }
}

