package server

import (
	"context"
	"crypto/tls"
	"errors"
	"fmt"
	"log"
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

func (s *TCPServer) createListener() (net.Listener, error) {
	if !s.cfg.TLSEnabled {
		return net.Listen("tcp", s.cfg.ListenAddr)
	}

	if s.cfg.TLSCertFile == "" || s.cfg.TLSKeyFile == "" {
		return nil, fmt.Errorf("tls enabled but cert/key file is missing")
	}

	certificate, err := tls.LoadX509KeyPair(s.cfg.TLSCertFile, s.cfg.TLSKeyFile)
	if err != nil {
		return nil, fmt.Errorf("load tls key pair: %w", err)
	}

	tlsConfig := &tls.Config{
		MinVersion:   tls.VersionTLS12,
		Certificates: []tls.Certificate{certificate},
	}
	return tls.Listen("tcp", s.cfg.ListenAddr, tlsConfig)
}

func (s *TCPServer) Run(ctx context.Context) error {
	listener, err := s.createListener()
	if err != nil {
		return err
	}
	defer listener.Close()
	log.Printf("signaling tcp listening addr=%s tls=%t auth_timeout=%s read_timeout=%s write_timeout=%s", s.cfg.ListenAddr, s.cfg.TLSEnabled, s.cfg.AuthTimeout, s.cfg.ReadTimeout, s.cfg.WriteTimeout)

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
		log.Printf("signaling accepted session=%d remote=%s", id, conn.RemoteAddr())
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
