package server

import (
    "encoding/json"
    "errors"
    "io"
    "meeting-server/signaling/config"
    "meeting-server/signaling/protocol"
    "net"
    "sync"
    "sync/atomic"
    "time"
)

var (
    ErrSessionClosed    = errors.New("session closed")
    ErrWriteBufferFull  = errors.New("session write buffer full")
)

type SessionState int32

const (
    StateConnected SessionState = iota
    StateAuthenticated
    StateInMeeting
    StateDisconnected
)

type Session struct {
    ID uint64

    conn net.Conn
    cfg  config.Config

    state atomic.Int32

    writeCh   chan []byte
    closeCh   chan struct{}
    closeOnce sync.Once

    connectedAt time.Time
    lastSeenNs  atomic.Int64

    metaMu    sync.RWMutex
    userID    string
    meetingID string
}

func NewSession(id uint64, conn net.Conn, cfg config.Config) *Session {
    s := &Session{
        ID:          id,
        conn:        conn,
        cfg:         cfg,
        writeCh:     make(chan []byte, cfg.WriteQueueSize),
        closeCh:     make(chan struct{}),
        connectedAt: time.Now(),
    }
    s.state.Store(int32(StateConnected))
    s.lastSeenNs.Store(time.Now().UnixNano())
    return s
}

func (s *Session) Start(onMessage func(*Session, protocol.SignalType, []byte), onClosed func(*Session)) {
    go s.readLoop(onMessage)
    go s.writeLoop()
    go s.watchdogLoop()

    go func() {
        <-s.closeCh
        onClosed(s)
    }()
}

func (s *Session) readLoop(onMessage func(*Session, protocol.SignalType, []byte)) {
    defer s.Close()

    header := make([]byte, protocol.HeaderSize)
    for {
        _ = s.conn.SetReadDeadline(time.Now().Add(s.cfg.ReadTimeout))
        if _, err := io.ReadFull(s.conn, header); err != nil {
            return
        }

        msgType, payloadLen, err := protocol.DecodeHeader(header, s.cfg.MaxPayloadBytes)
        if err != nil {
            return
        }

        payload := make([]byte, payloadLen)
        if payloadLen > 0 {
            if _, err := io.ReadFull(s.conn, payload); err != nil {
                return
            }
        }

        s.touch()
        onMessage(s, msgType, payload)
    }
}

func (s *Session) writeLoop() {
    defer s.Close()

    for {
        select {
        case data := <-s.writeCh:
            _ = s.conn.SetWriteDeadline(time.Now().Add(s.cfg.WriteTimeout))
            if _, err := s.conn.Write(data); err != nil {
                return
            }
        case <-s.closeCh:
            return
        }
    }
}

func (s *Session) watchdogLoop() {
    ticker := time.NewTicker(5 * time.Second)
    defer ticker.Stop()

    for {
        select {
        case <-ticker.C:
            if s.State() == StateConnected && time.Since(s.connectedAt) > s.cfg.AuthTimeout {
                s.Close()
                return
            }

            lastSeen := time.Unix(0, s.lastSeenNs.Load())
            if time.Since(lastSeen) > s.cfg.IdleTimeout {
                s.Close()
                return
            }
        case <-s.closeCh:
            return
        }
    }
}

func (s *Session) Send(msgType protocol.SignalType, body any) error {
    var payload []byte
    if body != nil {
        b, err := json.Marshal(body)
        if err != nil {
            return err
        }
        payload = b
    }

    return s.SendRaw(protocol.EncodeFrame(msgType, payload))
}

func (s *Session) SendRaw(frame []byte) error {
    select {
    case s.writeCh <- frame:
        return nil
    case <-s.closeCh:
        return ErrSessionClosed
    default:
        s.Close()
        return ErrWriteBufferFull
    }
}

func (s *Session) Close() {
    s.closeOnce.Do(func() {
        s.state.Store(int32(StateDisconnected))
        close(s.closeCh)
        _ = s.conn.Close()
    })
}

func (s *Session) State() SessionState {
    return SessionState(s.state.Load())
}

func (s *Session) SetState(state SessionState) {
    s.state.Store(int32(state))
}

func (s *Session) SetUserID(userID string) {
    s.metaMu.Lock()
    s.userID = userID
    s.metaMu.Unlock()
}

func (s *Session) UserID() string {
    s.metaMu.RLock()
    defer s.metaMu.RUnlock()
    return s.userID
}

func (s *Session) SetMeetingID(meetingID string) {
    s.metaMu.Lock()
    s.meetingID = meetingID
    s.metaMu.Unlock()
}

func (s *Session) MeetingID() string {
    s.metaMu.RLock()
    defer s.metaMu.RUnlock()
    return s.meetingID
}

func (s *Session) touch() {
    s.lastSeenNs.Store(time.Now().UnixNano())
}

