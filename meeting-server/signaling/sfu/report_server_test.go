package sfu

import (
	"context"
	"net"
	"testing"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/store"

	"github.com/alicebob/miniredis/v2"
	"google.golang.org/protobuf/proto"
)

func reserveLoopbackAddr(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve loopback addr failed: %v", err)
	}
	addr := listener.Addr().String()
	_ = listener.Close()
	return addr
}

func TestReportServerStoresActiveNodeRegistrationAndRuntime(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUStatusTTL = time.Minute
	cfg.SFUReportListenAddr = reserveLoopbackAddr(t)

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	server := NewReportServer(cfg, roomStore)
	if err := server.Start(ctx); err != nil {
		t.Fatalf("start report server failed: %v", err)
	}

	conn, err := net.DialTimeout("tcp", cfg.SFUReportListenAddr, time.Second)
	if err != nil {
		t.Fatalf("dial report server failed: %v", err)
	}
	defer conn.Close()

	payload, err := proto.Marshal(&pb.ReportNodeStatusReq{
		NodeId:         "sfu-live-01",
		RpcAddress:     "127.0.0.1:19000",
		SfuAddress:     "10.0.0.8:20000",
		MaxMeetings:    16,
		MediaPort:      20000,
		RoomCount:      3,
		PublisherCount: 9,
		PacketCount:    1234,
	})
	if err != nil {
		t.Fatalf("marshal report request failed: %v", err)
	}

	if err := writeFrame(conn, wireHeader{
		method: methodReportNodeStatus,
		kind:   wireKindRequest,
		status: 0,
		length: uint32(len(payload)),
	}, payload); err != nil {
		t.Fatalf("write report frame failed: %v", err)
	}

	header, body, err := readFrame(conn)
	if err != nil {
		t.Fatalf("read report response failed: %v", err)
	}
	if header.method != methodReportNodeStatus || header.kind != wireKindResponse || header.status != 0 {
		t.Fatalf("unexpected response header: %+v", header)
	}

	rsp := &pb.ReportNodeStatusRsp{}
	if err := proto.Unmarshal(body, rsp); err != nil {
		t.Fatalf("unmarshal report response failed: %v", err)
	}
	if !rsp.GetSuccess() {
		t.Fatalf("unexpected report response: %+v", rsp)
	}

	nodes, err := roomStore.RegisteredSFUNodes(context.Background())
	if err != nil {
		t.Fatalf("load registered nodes failed: %v", err)
	}
	if len(nodes) != 1 || nodes[0].NodeID != "sfu-live-01" || nodes[0].RPCAddress != "127.0.0.1:19000" || nodes[0].MediaAddress != "10.0.0.8:20000" || nodes[0].MaxMeetings != 16 {
		t.Fatalf("unexpected registered nodes: %+v", nodes)
	}

	status, err := roomStore.SFUNodeStatus(context.Background(), "sfu-live-01")
	if err != nil {
		t.Fatalf("load node status failed: %v", err)
	}
	if status == nil || status.RoomCount != 3 || status.PublisherCount != 9 || status.PacketCount != 1234 {
		t.Fatalf("unexpected node status: %+v", status)
	}

	rpcAddr, err := roomStore.RPCAddressForRoute(context.Background(), "10.0.0.8:20000")
	if err != nil {
		t.Fatalf("load route rpc mapping failed: %v", err)
	}
	if rpcAddr != "127.0.0.1:19000" {
		t.Fatalf("unexpected route rpc mapping: %q", rpcAddr)
	}
}

func TestReportServerStoresParticipantQualitySnapshots(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUStatusTTL = time.Minute
	cfg.SFUReportListenAddr = reserveLoopbackAddr(t)

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	server := NewReportServer(cfg, roomStore)
	if err := server.Start(ctx); err != nil {
		t.Fatalf("start report server failed: %v", err)
	}

	participant := protocol.Participant{UserId: "u7001", DisplayName: "Alice"}
	if err := roomStore.AddMember(context.Background(), "m7001", &participant); err != nil {
		t.Fatalf("seed member failed: %v", err)
	}

	conn, err := net.DialTimeout("tcp", cfg.SFUReportListenAddr, time.Second)
	if err != nil {
		t.Fatalf("dial report server failed: %v", err)
	}
	defer conn.Close()

	payload, err := proto.Marshal(&pb.QualityReport{
		MeetingId:   "m7001",
		UserId:      "u7001",
		PacketLoss:  0.2,
		RttMs:       91,
		JitterMs:    14,
		BitrateKbps: 880,
	})
	if err != nil {
		t.Fatalf("marshal quality request failed: %v", err)
	}

	if err := writeFrame(conn, wireHeader{
		method: methodQualityReport,
		kind:   wireKindRequest,
		status: 0,
		length: uint32(len(payload)),
	}, payload); err != nil {
		t.Fatalf("write quality frame failed: %v", err)
	}

	header, body, err := readFrame(conn)
	if err != nil {
		t.Fatalf("read quality response failed: %v", err)
	}
	if header.method != methodQualityReport || header.kind != wireKindResponse || header.status != 0 {
		t.Fatalf("unexpected quality response header: %+v", header)
	}
	if len(body) != 0 {
		t.Fatalf("expected empty quality response body, got %d bytes", len(body))
	}

	quality, err := roomStore.LoadParticipantQuality(context.Background(), "m7001", "u7001")
	if err != nil {
		t.Fatalf("load participant quality failed: %v", err)
	}
	if quality == nil || quality.RttMs != 91 || quality.JitterMs != 14 || quality.BitrateKbps != 880 {
		t.Fatalf("unexpected participant quality: %+v", quality)
	}
}
