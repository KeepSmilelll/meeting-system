package sfu

import (
	"bytes"
	"context"
	"errors"
	"net"
	"testing"
	"time"

	"meeting-server/signaling/protocol/pb"

	"google.golang.org/protobuf/proto"
)

func TestFrameRoundTrip(t *testing.T) {
	payload := []byte("hello rpc")
	header := wireHeader{
		method: methodAddPublisher,
		kind:   wireKindResponse,
		status: 7,
		length: uint32(len(payload)),
	}

	var buf bytes.Buffer
	if err := writeFrame(&buf, header, payload); err != nil {
		t.Fatalf("writeFrame failed: %v", err)
	}

	gotHeader, gotPayload, err := readFrame(bytes.NewReader(buf.Bytes()))
	if err != nil {
		t.Fatalf("readFrame failed: %v", err)
	}

	if gotHeader.method != header.method {
		t.Fatalf("unexpected method: got %d want %d", gotHeader.method, header.method)
	}
	if gotHeader.kind != header.kind {
		t.Fatalf("unexpected kind: got %d want %d", gotHeader.kind, header.kind)
	}
	if gotHeader.status != header.status {
		t.Fatalf("unexpected status: got %d want %d", gotHeader.status, header.status)
	}
	if !bytes.Equal(gotPayload, payload) {
		t.Fatalf("unexpected payload: got %q want %q", gotPayload, payload)
	}
}

func TestTCPClientCreateRoomRoundTrip(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	client := NewClient("127.0.0.1:10000", WithTimeout(time.Second), WithDialer(func(context.Context, string, string) (net.Conn, error) {
		return clientConn, nil
	}))

	done := make(chan struct{})
	go func() {
		defer close(done)
		defer serverConn.Close()

		header, payload, err := readFrame(serverConn)
		if err != nil {
			t.Errorf("readFrame failed: %v", err)
			return
		}
		if header.method != methodCreateRoom {
			t.Errorf("unexpected method: got %d want %d", header.method, methodCreateRoom)
			return
		}
		if header.kind != wireKindRequest {
			t.Errorf("unexpected kind: got %d want %d", header.kind, wireKindRequest)
			return
		}

		req := &pb.CreateRoomReq{}
		if err := proto.Unmarshal(payload, req); err != nil {
			t.Errorf("unmarshal request failed: %v", err)
			return
		}
		if req.GetMeetingId() != "meeting-01" || req.GetMaxPublishers() != 4 {
			t.Errorf("unexpected request: %+v", req)
			return
		}

		rspBytes, err := proto.Marshal(&pb.CreateRoomRsp{Success: true, SfuAddress: "127.0.0.1:10000"})
		if err != nil {
			t.Errorf("marshal response failed: %v", err)
			return
		}

		if err := writeFrame(serverConn, wireHeader{
			method: methodCreateRoom,
			kind:   wireKindResponse,
			status: 0,
			length: uint32(len(rspBytes)),
		}, rspBytes); err != nil {
			t.Errorf("writeFrame failed: %v", err)
			return
		}
	}()

	rsp, err := client.CreateRoom(context.Background(), &pb.CreateRoomReq{MeetingId: "meeting-01", MaxPublishers: 4})
	if err != nil {
		t.Fatalf("CreateRoom failed: %v", err)
	}
	if !rsp.GetSuccess() {
		t.Fatalf("unexpected success flag: %+v", rsp)
	}
	if rsp.GetSfuAddress() != "127.0.0.1:10000" {
		t.Fatalf("unexpected sfu address: %+v", rsp)
	}

	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("server goroutine did not finish")
	}
}

func TestDisabledClient(t *testing.T) {
	client := NewClient("")
	rsp, err := client.DestroyRoom(context.Background(), &pb.DestroyRoomReq{MeetingId: "meeting-01"})
	if !errors.Is(err, ErrDisabled) {
		t.Fatalf("unexpected error: %v", err)
	}
	if rsp == nil || rsp.GetSuccess() {
		t.Fatalf("unexpected disabled response: %+v", rsp)
	}
}
