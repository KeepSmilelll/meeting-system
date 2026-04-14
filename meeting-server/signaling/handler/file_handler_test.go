package handler

import (
	"testing"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"

	"google.golang.org/protobuf/proto"
)

func TestFileHandlerOfferRejectedWhenDisabled(t *testing.T) {
	cfg := config.Load()

	session, client, cleanup := newRunningSession(t, 901, cfg)
	defer cleanup()
	session.SetState(server.StateInMeeting)

	handler := NewFileHandler(cfg, server.NewSessionManager())
	payload, err := proto.Marshal(&protocol.FileOfferReqBody{
		FileName: "demo.txt",
		FileSize: 1024,
		FileHash: "abc",
	})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}

	handler.HandleOffer(session, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.FileOfferRsp {
		t.Fatalf("unexpected message type: got %v want %v", msgType, protocol.FileOfferRsp)
	}

	var rsp protocol.FileOfferRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal file offer rsp failed: %v", err)
	}

	if rsp.Success {
		t.Fatalf("expected disabled file transfer response")
	}
	if rsp.Error == nil || rsp.Error.Code != protocol.ErrTransferRejected {
		t.Fatalf("unexpected error payload: %+v", rsp.Error)
	}
	if rsp.TransferId == "" {
		t.Fatalf("expected non-empty transfer id for audit trace")
	}
}

func TestFileHandlerOfferValidatesPayload(t *testing.T) {
	cfg := config.Load()

	session, client, cleanup := newRunningSession(t, 902, cfg)
	defer cleanup()
	session.SetState(server.StateInMeeting)

	handler := NewFileHandler(cfg, server.NewSessionManager())
	payload, err := proto.Marshal(&protocol.FileOfferReqBody{})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}

	handler.HandleOffer(session, payload)

	msgType, rspPayload := readFrame(t, client, cfg.MaxPayloadBytes)
	if msgType != protocol.FileOfferRsp {
		t.Fatalf("unexpected message type: got %v want %v", msgType, protocol.FileOfferRsp)
	}

	var rsp protocol.FileOfferRspBody
	if err := proto.Unmarshal(rspPayload, &rsp); err != nil {
		t.Fatalf("unmarshal file offer rsp failed: %v", err)
	}

	if rsp.Success {
		t.Fatalf("expected invalid payload rejection")
	}
	if rsp.Error == nil || rsp.Error.Code != protocol.ErrInvalidParam {
		t.Fatalf("unexpected error payload: %+v", rsp.Error)
	}
}
