package sfu

import (
	"context"
	"errors"
	"testing"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/store"

	"github.com/alicebob/miniredis/v2"
)

type stubNodeStatusClient struct {
	getNodeStatusRsp *pb.GetNodeStatusRsp
	getNodeStatusErr error
}

func (c *stubNodeStatusClient) CreateRoom(context.Context, *pb.CreateRoomReq) (*pb.CreateRoomRsp, error) {
	return &pb.CreateRoomRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) DestroyRoom(context.Context, *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error) {
	return &pb.DestroyRoomRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) SetupTransport(context.Context, *pb.SetupTransportReq) (*pb.SetupTransportRsp, error) {
	return &pb.SetupTransportRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) TrickleIceCandidate(context.Context, *pb.TrickleIceCandidateReq) (*pb.TrickleIceCandidateRsp, error) {
	return &pb.TrickleIceCandidateRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) CloseTransport(context.Context, *pb.CloseTransportReq) (*pb.CloseTransportRsp, error) {
	return &pb.CloseTransportRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) AddPublisher(context.Context, *pb.AddPublisherReq) (*pb.AddPublisherRsp, error) {
	return &pb.AddPublisherRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) AddSubscriber(context.Context, *pb.AddSubscriberReq) (*pb.AddSubscriberRsp, error) {
	return &pb.AddSubscriberRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) RemovePublisher(context.Context, *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error) {
	return &pb.RemovePublisherRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) RemoveSubscriber(context.Context, *pb.RemoveSubscriberReq) (*pb.RemoveSubscriberRsp, error) {
	return &pb.RemoveSubscriberRsp{Success: true}, nil
}

func (c *stubNodeStatusClient) GetNodeStatus(context.Context, *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error) {
	if c.getNodeStatusRsp != nil || c.getNodeStatusErr != nil {
		if c.getNodeStatusRsp == nil {
			return &pb.GetNodeStatusRsp{}, c.getNodeStatusErr
		}
		return c.getNodeStatusRsp, c.getNodeStatusErr
	}
	return &pb.GetNodeStatusRsp{Success: true}, nil
}

func TestNodeStatusMonitorPollOnceReportsRuntimeAndQuarantinesFailures(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "media-b:10000", RPCAddress: "rpc-b:9000", MaxMeetings: 10},
	}

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()

	ctx := context.Background()
	if err := roomStore.MarkSFUNodeFailure(ctx, "sfu-a", time.Minute); err != nil {
		t.Fatalf("mark initial quarantine failed: %v", err)
	}

	monitor := NewNodeStatusMonitorWithClients(cfg, roomStore, map[string]Client{
		"sfu-a": &stubNodeStatusClient{
			getNodeStatusRsp: &pb.GetNodeStatusRsp{
				Success:        true,
				SfuAddress:     "media-a:10000",
				MediaPort:      5004,
				RoomCount:      2,
				PublisherCount: 5,
				PacketCount:    42,
			},
		},
		"sfu-b": &stubNodeStatusClient{getNodeStatusErr: errors.New("dial failed")},
	})

	monitor.PollOnce(ctx)

	status, err := roomStore.SFUNodeStatus(ctx, "sfu-a")
	if err != nil {
		t.Fatalf("load node status failed: %v", err)
	}
	if status == nil {
		t.Fatal("expected persisted node status for sfu-a")
	}
	if status.RoomCount != 2 || status.PublisherCount != 5 || status.PacketCount != 42 {
		t.Fatalf("unexpected persisted node status: %+v", status)
	}
	if mini.Exists("sfu_node_quarantine:sfu-a") {
		t.Fatal("expected successful poll to clear prior quarantine")
	}
	if !mini.Exists("sfu_node_quarantine:sfu-b") {
		t.Fatal("expected failed poll to quarantine sfu-b")
	}

	ranked, err := roomStore.RankedSFUNodes(ctx, cfg.SFUNodes)
	if err != nil {
		t.Fatalf("rank nodes failed: %v", err)
	}
	if len(ranked) != 2 || ranked[0].NodeID != "sfu-a" || ranked[1].NodeID != "sfu-b" {
		t.Fatalf("unexpected ranked nodes after poll: %+v", ranked)
	}
}

func TestNodeStatusMonitorPollOnceIncludesRegisteredNodes(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "media-a:10000", RPCAddress: "rpc-a:9000", MaxMeetings: 10},
	}

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()

	ctx := context.Background()
	if err := roomStore.ReportSFUNode(ctx, config.SFUNode{
		NodeID:       "sfu-b",
		MediaAddress: "media-b:10000",
		RPCAddress:   "rpc-b:9000",
		MaxMeetings:  20,
	}, store.SFUNodeStatus{
		SFUAddress:     "media-b:10000",
		MediaPort:      5004,
		RoomCount:      1,
		PublisherCount: 1,
		PacketCount:    10,
	}, time.Minute); err != nil {
		t.Fatalf("seed registered node status failed: %v", err)
	}

	monitor := NewNodeStatusMonitorWithClients(cfg, roomStore, map[string]Client{
		"sfu-a": &stubNodeStatusClient{getNodeStatusRsp: &pb.GetNodeStatusRsp{
			Success:        true,
			SfuAddress:     "media-a:10000",
			MediaPort:      5004,
			RoomCount:      2,
			PublisherCount: 3,
			PacketCount:    11,
		}},
		"sfu-b": &stubNodeStatusClient{getNodeStatusRsp: &pb.GetNodeStatusRsp{
			Success:        true,
			SfuAddress:     "media-b:10000",
			MediaPort:      5006,
			RoomCount:      6,
			PublisherCount: 8,
			PacketCount:    77,
		}},
	})

	monitor.PollOnce(ctx)

	statusB, err := roomStore.SFUNodeStatus(ctx, "sfu-b")
	if err != nil {
		t.Fatalf("load node status sfu-b failed: %v", err)
	}
	if statusB == nil || statusB.PacketCount != 77 || statusB.RoomCount != 6 || statusB.PublisherCount != 8 {
		t.Fatalf("expected registered node sfu-b to be polled and updated, got %+v", statusB)
	}
}

func TestNodeStatusMonitorPollOnceUsesRegisteredNodesWithoutStaticConfig(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodes = nil

	roomStore := store.NewRedisRoomStore(cfg)
	defer roomStore.Close()

	ctx := context.Background()
	if err := roomStore.ReportSFUNode(ctx, config.SFUNode{
		NodeID:       "sfu-live-01",
		MediaAddress: "10.0.0.8:20000",
		RPCAddress:   "127.0.0.1:19000",
		MaxMeetings:  16,
	}, store.SFUNodeStatus{
		SFUAddress:     "10.0.0.8:20000",
		MediaPort:      20000,
		RoomCount:      3,
		PublisherCount: 9,
		PacketCount:    100,
	}, time.Minute); err != nil {
		t.Fatalf("seed registered node failed: %v", err)
	}

	monitor := NewNodeStatusMonitorWithClients(cfg, roomStore, map[string]Client{
		"sfu-live-01": &stubNodeStatusClient{getNodeStatusRsp: &pb.GetNodeStatusRsp{
			Success:        true,
			SfuAddress:     "10.0.0.8:20000",
			MediaPort:      20000,
			RoomCount:      4,
			PublisherCount: 10,
			PacketCount:    188,
		}},
	})

	monitor.PollOnce(ctx)

	status, err := roomStore.SFUNodeStatus(ctx, "sfu-live-01")
	if err != nil {
		t.Fatalf("load node status failed: %v", err)
	}
	if status == nil || status.PacketCount != 188 || status.RoomCount != 4 || status.PublisherCount != 10 {
		t.Fatalf("expected registered node to be polled without static config, got %+v", status)
	}
}
