package server

import (
	"context"
	"io"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"net"
	"testing"

	"google.golang.org/protobuf/proto"
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

type recordingMeetingPublisher struct {
	meetingID     string
	frame         []byte
	excludeUserID string
	calls         int
}

func (p *recordingMeetingPublisher) PublishMeetingFrame(_ context.Context, meetingID string, frame []byte, excludeUserID string) error {
	p.calls++
	p.meetingID = meetingID
	p.frame = append([]byte(nil), frame...)
	p.excludeUserID = excludeUserID
	return nil
}

func TestBroadcastToMeetingPublishesAndDeliversLocally(t *testing.T) {
	cfg := config.Load()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	session := NewSession(1, serverConn, cfg)
	session.SetUserID("u1001")
	session.SetMeetingID("m1001")
	go session.writeLoop()
	defer session.Close()

	mgr := NewSessionManager()
	mgr.Add(session)

	publisher := &recordingMeetingPublisher{}
	mgr.SetMeetingFramePublisher(publisher)

	body := &protocol.ChatRecvNotifyBody{MessageId: "msg-1", SenderId: "u1002", Content: "hello"}
	mgr.BroadcastToMeeting("m1001", protocol.ChatRecvNotify, body, "")

	msgType, payload := readFrame(t, clientConn, cfg.MaxPayloadBytes)
	if msgType != protocol.ChatRecvNotify {
		t.Fatalf("unexpected local msg type: got %v want %v", msgType, protocol.ChatRecvNotify)
	}

	var notify protocol.ChatRecvNotifyBody
	if err := proto.Unmarshal(payload, &notify); err != nil {
		t.Fatalf("unmarshal local notify failed: %v", err)
	}
	if notify.Content != "hello" || notify.SenderId != "u1002" {
		t.Fatalf("unexpected local notify: %+v", notify)
	}

	if publisher.calls != 1 {
		t.Fatalf("expected one publish call, got %d", publisher.calls)
	}
	if publisher.meetingID != "m1001" || publisher.excludeUserID != "" {
		t.Fatalf("unexpected publish metadata: %+v", publisher)
	}
}

func TestDeliverMeetingFrameDoesNotRepublish(t *testing.T) {
	cfg := config.Load()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	session := NewSession(2, serverConn, cfg)
	session.SetUserID("u1001")
	session.SetMeetingID("m1001")
	go session.writeLoop()
	defer session.Close()

	mgr := NewSessionManager()
	mgr.Add(session)

	publisher := &recordingMeetingPublisher{}
	mgr.SetMeetingFramePublisher(publisher)

	frame := protocol.EncodeFrame(protocol.MeetParticipantLeave, []byte{})
	mgr.DeliverMeetingFrame("m1001", frame, "")

	msgType, _ := readFrame(t, clientConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetParticipantLeave {
		t.Fatalf("unexpected delivered msg type: got %v want %v", msgType, protocol.MeetParticipantLeave)
	}
	if publisher.calls != 0 {
		t.Fatalf("expected deliver-only path to avoid publish, got %d", publisher.calls)
	}
}

func TestDeliverUserFrameTargetsBoundSession(t *testing.T) {
	cfg := config.Load()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	session := NewSession(3, serverConn, cfg)
	session.SetUserID("u1001")
	go session.writeLoop()
	defer session.Close()

	mgr := NewSessionManager()
	mgr.Add(session)
	mgr.BindUser(session, "u1001")

	frame := protocol.EncodeFrame(protocol.AuthHeartbeatRsp, []byte{})
	if !mgr.DeliverUserFrame("u1001", session.ID, frame) {
		t.Fatal("expected direct user frame delivery")
	}

	msgType, _ := readFrame(t, clientConn, cfg.MaxPayloadBytes)
	if msgType != protocol.AuthHeartbeatRsp {
		t.Fatalf("unexpected delivered direct frame type: got %v want %v", msgType, protocol.AuthHeartbeatRsp)
	}
}

func readFrame(t *testing.T, conn net.Conn, maxPayload uint32) (protocol.SignalType, []byte) {
	t.Helper()

	header := make([]byte, protocol.HeaderSize)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("read header failed: %v", err)
	}
	msgType, payloadLen, err := protocol.DecodeHeader(header, maxPayload)
	if err != nil {
		t.Fatalf("decode header failed: %v", err)
	}

	payload := make([]byte, payloadLen)
	if payloadLen > 0 {
		if _, err := io.ReadFull(conn, payload); err != nil {
			t.Fatalf("read payload failed: %v", err)
		}
	}
	return msgType, payload
}
