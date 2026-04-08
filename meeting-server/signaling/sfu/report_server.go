package sfu

import (
	"context"
	"fmt"
	"net"
	"sync"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

type ReportServer struct {
	cfg       config.Config
	roomState *store.RedisRoomStore

	mu       sync.Mutex
	listener net.Listener
}

func NewReportServer(cfg config.Config, roomState *store.RedisRoomStore) *ReportServer {
	return &ReportServer{
		cfg:       cfg,
		roomState: roomState,
	}
}

func (s *ReportServer) Start(ctx context.Context) error {
	if s == nil || s.roomState == nil || !s.roomState.Enabled() || s.cfg.SFUReportListenAddr == "" {
		return nil
	}

	listener, err := net.Listen("tcp", s.cfg.SFUReportListenAddr)
	if err != nil {
		return fmt.Errorf("sfu report server listen %s: %w", s.cfg.SFUReportListenAddr, err)
	}

	s.mu.Lock()
	s.listener = listener
	s.mu.Unlock()

	go func() {
		<-ctx.Done()
		s.close()
	}()

	go s.acceptLoop(ctx, listener)
	return nil
}

func (s *ReportServer) close() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.listener != nil {
		_ = s.listener.Close()
		s.listener = nil
	}
}

func (s *ReportServer) acceptLoop(ctx context.Context, listener net.Listener) {
	for {
		conn, err := listener.Accept()
		if err != nil {
			select {
			case <-ctx.Done():
				return
			default:
			}
			return
		}

		go func() {
			defer conn.Close()
			_ = s.handleConn(ctx, conn)
		}()
	}
}

func (s *ReportServer) handleConn(ctx context.Context, conn net.Conn) error {
	header, payload, err := readFrame(conn)
	if err != nil {
		return err
	}

	responseHeader := wireHeader{
		method: header.method,
		kind:   wireKindResponse,
		status: 1,
	}
	responsePayload := []byte{}

	if header.kind == wireKindRequest {
		switch header.method {
		case methodReportNodeStatus:
			req := &pb.ReportNodeStatusReq{}
			rsp := &pb.ReportNodeStatusRsp{Success: false}
			if err := proto.Unmarshal(payload, req); err == nil && s.reportNodeStatus(ctx, req, rsp) == nil {
				responseHeader.status = 0
			}
			responsePayload, _ = proto.Marshal(rsp)
			responseHeader.length = uint32(len(responsePayload))
		case methodQualityReport:
			req := &pb.QualityReport{}
			if err := proto.Unmarshal(payload, req); err == nil && s.reportQuality(ctx, req) == nil {
				responseHeader.status = 0
			}
		}
	}

	return writeFrame(conn, responseHeader, responsePayload)
}

func (s *ReportServer) reportNodeStatus(ctx context.Context, req *pb.ReportNodeStatusReq, rsp *pb.ReportNodeStatusRsp) error {
	if s == nil || s.roomState == nil || req == nil || rsp == nil || req.GetNodeId() == "" {
		return fmt.Errorf("invalid node status report")
	}

	err := s.roomState.ReportSFUNode(ctx, config.SFUNode{
		NodeID:       req.GetNodeId(),
		MediaAddress: req.GetSfuAddress(),
		RPCAddress:   req.GetRpcAddress(),
		MaxMeetings:  int(req.GetMaxMeetings()),
	}, store.SFUNodeStatus{
		SFUAddress:     req.GetSfuAddress(),
		MediaPort:      req.GetMediaPort(),
		RoomCount:      int(req.GetRoomCount()),
		PublisherCount: int(req.GetPublisherCount()),
		PacketCount:    req.GetPacketCount(),
	}, s.cfg.SFUStatusTTL)
	if err != nil {
		return err
	}

	_ = s.roomState.ClearSFUNodeFailure(ctx, req.GetNodeId())
	rsp.Success = true
	return nil
}
func (s *ReportServer) reportQuality(ctx context.Context, req *pb.QualityReport) error {
	if s == nil || s.roomState == nil || req == nil || req.GetMeetingId() == "" || req.GetUserId() == "" {
		return fmt.Errorf("invalid quality report")
	}

	return s.roomState.ReportParticipantQuality(ctx, req.GetMeetingId(), req.GetUserId(), store.ParticipantQuality{
		PacketLoss:  req.GetPacketLoss(),
		RttMs:       req.GetRttMs(),
		JitterMs:    req.GetJitterMs(),
		BitrateKbps: req.GetBitrateKbps(),
	})
}
