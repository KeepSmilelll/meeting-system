package server

import (
    "meeting-server/signaling/config"
    "net"
    "testing"
)

func TestBindUserKicksOldSession(t *testing.T) {
    cfg := config.Load()

    c1, c2 := net.Pipe()
    defer c1.Close()
    defer c2.Close()

    c3, c4 := net.Pipe()
    defer c3.Close()
    defer c4.Close()

    s1 := NewSession(1, c1, cfg)
    s2 := NewSession(2, c3, cfg)

    mgr := NewSessionManager()
    mgr.Add(s1)
    mgr.Add(s2)

    mgr.BindUser(s1, "u1001")
    mgr.BindUser(s2, "u1001")

    got, ok := mgr.GetByUser("u1001")
    if !ok {
        t.Fatalf("expected user binding")
    }
    if got.ID != s2.ID {
        t.Fatalf("expected second session to be bound")
    }
    if s1.State() != StateDisconnected {
        t.Fatalf("expected first session closed, got state %v", s1.State())
    }
}

