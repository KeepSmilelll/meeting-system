package handler

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"meeting-server/signaling/auth"
	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	pb "meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/server"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"

	"github.com/alicebob/miniredis/v2"
	"google.golang.org/protobuf/proto"
)

func assertNoFrame(t *testing.T, conn net.Conn, label string) {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	buf := make([]byte, 1)
	_, err := conn.Read(buf)
	if err == nil {
		t.Fatalf("expected no frame on %s, but received data", label)
	}
	if ne, ok := err.(net.Error); !ok || !ne.Timeout() {
		if err != io.EOF {
			t.Fatalf("expected timeout on %s, got %v", label, err)
		}
	}
}

type stubSFUClient struct {
	mu sync.Mutex

	createRoomRequests   []*pb.CreateRoomReq
	createRoomResponse   *pb.CreateRoomRsp
	createRoomErr        error
	setupTransportReqs   []*pb.SetupTransportReq
	setupTransportRsp    *pb.SetupTransportRsp
	setupTransportErr    error
	trickleIceReqs       []*pb.TrickleIceCandidateReq
	trickleIceRsp        *pb.TrickleIceCandidateRsp
	trickleIceErr        error
	closeTransportReqs   []*pb.CloseTransportReq
	closeTransportRsp    *pb.CloseTransportRsp
	closeTransportErr    error
	requests             []*pb.AddPublisherReq
	addSubscriberReqs    []*pb.AddSubscriberReq
	removePublisherReqs  []*pb.RemovePublisherReq
	removeSubscriberReqs []*pb.RemoveSubscriberReq
	destroyRoomReqs      []*pb.DestroyRoomReq
	addPublisherRsp      *pb.AddPublisherRsp
	addPublisherErr      error
	addPublisherHook     func(*pb.AddPublisherReq)
	addSubscriberRsp     *pb.AddSubscriberRsp
	addSubscriberErr     error
	removePublisherRsp   *pb.RemovePublisherRsp
	removePublisherErr   error
	removeSubscriberRsp  *pb.RemoveSubscriberRsp
	removeSubscriberErr  error
	destroyRoomRsp       *pb.DestroyRoomRsp
	destroyRoomErr       error
	getNodeStatusRsp     *pb.GetNodeStatusRsp
	getNodeStatusErr     error
}

type stubSessionStore struct {
	presence *store.SessionPresence
}

func (s *stubSessionStore) Upsert(context.Context, store.SessionPresence, time.Duration) error {
	return nil
}

func (s *stubSessionStore) Get(context.Context, string) (*store.SessionPresence, error) {
	if s.presence == nil {
		return nil, nil
	}
	copy := *s.presence
	return &copy, nil
}

func (s *stubSessionStore) Delete(context.Context, string, uint64) error {
	return nil
}

type stubUserFramePublisher struct {
	nodeID          string
	userID          string
	targetSessionID uint64
	frame           []byte
	calls           int
	controls        []store.UserNodeEvent
}

func (p *stubUserFramePublisher) PublishUserFrame(_ context.Context, nodeID, userID string, targetSessionID uint64, frame []byte) error {
	p.calls++
	p.nodeID = nodeID
	p.userID = userID
	p.targetSessionID = targetSessionID
	p.frame = append([]byte(nil), frame...)
	return nil
}

func (p *stubUserFramePublisher) PublishUserControl(_ context.Context, event store.UserNodeEvent) error {
	p.controls = append(p.controls, event)
	return nil
}

func (c *stubSFUClient) CreateRoom(_ context.Context, req *pb.CreateRoomReq) (*pb.CreateRoomRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.createRoomRequests = append(c.createRoomRequests, &clone)
	if c.createRoomResponse != nil || c.createRoomErr != nil {
		if c.createRoomResponse == nil {
			return &pb.CreateRoomRsp{}, c.createRoomErr
		}
		return c.createRoomResponse, c.createRoomErr
	}
	return &pb.CreateRoomRsp{Success: true}, nil
}

func (c *stubSFUClient) DestroyRoom(_ context.Context, req *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.destroyRoomReqs = append(c.destroyRoomReqs, &clone)
	if c.destroyRoomRsp != nil || c.destroyRoomErr != nil {
		if c.destroyRoomRsp == nil {
			return &pb.DestroyRoomRsp{}, c.destroyRoomErr
		}
		return c.destroyRoomRsp, c.destroyRoomErr
	}
	return &pb.DestroyRoomRsp{Success: true}, nil
}

func (c *stubSFUClient) SetupTransport(_ context.Context, req *pb.SetupTransportReq) (*pb.SetupTransportRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	clone.ClientCandidates = append([]string(nil), req.GetClientCandidates()...)
	c.setupTransportReqs = append(c.setupTransportReqs, &clone)
	if c.setupTransportRsp != nil || c.setupTransportErr != nil {
		if c.setupTransportRsp == nil {
			return &pb.SetupTransportRsp{}, c.setupTransportErr
		}
		return c.setupTransportRsp, c.setupTransportErr
	}
	return &pb.SetupTransportRsp{
		Success:               true,
		ServerIceUfrag:        "serverUfrag",
		ServerIcePwd:          "serverPwd",
		ServerDtlsFingerprint: "AA:BB",
		ServerCandidates:      []string{"candidate:1 1 udp 2130706431 127.0.0.1 5004 typ host"},
		AssignedAudioSsrc:     1111,
		AssignedVideoSsrc:     2222,
	}, nil
}

func (c *stubSFUClient) TrickleIceCandidate(_ context.Context, req *pb.TrickleIceCandidateReq) (*pb.TrickleIceCandidateRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.trickleIceReqs = append(c.trickleIceReqs, &clone)
	if c.trickleIceRsp != nil || c.trickleIceErr != nil {
		if c.trickleIceRsp == nil {
			return &pb.TrickleIceCandidateRsp{}, c.trickleIceErr
		}
		return c.trickleIceRsp, c.trickleIceErr
	}
	return &pb.TrickleIceCandidateRsp{Success: true}, nil
}

func (c *stubSFUClient) CloseTransport(_ context.Context, req *pb.CloseTransportReq) (*pb.CloseTransportRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.closeTransportReqs = append(c.closeTransportReqs, &clone)
	if c.closeTransportRsp != nil || c.closeTransportErr != nil {
		if c.closeTransportRsp == nil {
			return &pb.CloseTransportRsp{}, c.closeTransportErr
		}
		return c.closeTransportRsp, c.closeTransportErr
	}
	return &pb.CloseTransportRsp{Success: true}, nil
}

func (c *stubSFUClient) AddPublisher(_ context.Context, req *pb.AddPublisherReq) (*pb.AddPublisherRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.requests = append(c.requests, &clone)
	if c.addPublisherHook != nil {
		c.addPublisherHook(&clone)
	}
	if c.addPublisherRsp != nil || c.addPublisherErr != nil {
		if c.addPublisherRsp == nil {
			return &pb.AddPublisherRsp{}, c.addPublisherErr
		}
		return c.addPublisherRsp, c.addPublisherErr
	}
	return &pb.AddPublisherRsp{Success: true}, nil
}

func (c *stubSFUClient) AddSubscriber(_ context.Context, req *pb.AddSubscriberReq) (*pb.AddSubscriberRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.addSubscriberReqs = append(c.addSubscriberReqs, &clone)
	if c.addSubscriberRsp != nil || c.addSubscriberErr != nil {
		if c.addSubscriberRsp == nil {
			return &pb.AddSubscriberRsp{}, c.addSubscriberErr
		}
		return c.addSubscriberRsp, c.addSubscriberErr
	}
	return &pb.AddSubscriberRsp{Success: true}, nil
}

func (c *stubSFUClient) RemovePublisher(_ context.Context, req *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.removePublisherReqs = append(c.removePublisherReqs, &clone)
	if c.removePublisherRsp != nil || c.removePublisherErr != nil {
		if c.removePublisherRsp == nil {
			return &pb.RemovePublisherRsp{}, c.removePublisherErr
		}
		return c.removePublisherRsp, c.removePublisherErr
	}
	return &pb.RemovePublisherRsp{Success: true}, nil
}

func (c *stubSFUClient) RemoveSubscriber(_ context.Context, req *pb.RemoveSubscriberReq) (*pb.RemoveSubscriberRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	clone := *req
	c.removeSubscriberReqs = append(c.removeSubscriberReqs, &clone)
	if c.removeSubscriberRsp != nil || c.removeSubscriberErr != nil {
		if c.removeSubscriberRsp == nil {
			return &pb.RemoveSubscriberRsp{}, c.removeSubscriberErr
		}
		return c.removeSubscriberRsp, c.removeSubscriberErr
	}
	return &pb.RemoveSubscriberRsp{Success: true}, nil
}

func (c *stubSFUClient) GetNodeStatus(_ context.Context, req *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error) {
	c.mu.Lock()
	defer c.mu.Unlock()

	_ = req
	if c.getNodeStatusRsp != nil || c.getNodeStatusErr != nil {
		if c.getNodeStatusRsp == nil {
			return &pb.GetNodeStatusRsp{}, c.getNodeStatusErr
		}
		return c.getNodeStatusRsp, c.getNodeStatusErr
	}
	return &pb.GetNodeStatusRsp{Success: true}, nil
}

func TestRouterSetsUpMediaTransportAndTricklesIce(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sfuClient := &stubSFUClient{}
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	meeting, hostParticipant, err := memStore.CreateMeeting("media-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}

	senderSess, senderConn, senderCleanup := newRunningSession(t, 301, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{
		MeetingId:             meeting.ID,
		PublishAudio:          true,
		PublishVideo:          true,
		ClientIceUfrag:        "clientUfrag",
		ClientIcePwd:          "clientPwd",
		ClientDtlsFingerprint: "AA:BB",
		ClientCandidates:      []string{"candidate:1 1 udp 2130706431 127.0.0.1 5004 typ host"},
	})
	if err != nil {
		t.Fatalf("marshal transport offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	if len(sfuClient.setupTransportReqs) != 1 {
		t.Fatalf("expected one setup transport call, got %d", len(sfuClient.setupTransportReqs))
	}
	if got := sfuClient.setupTransportReqs[0]; got.GetMeetingId() != meeting.ID || got.GetUserId() != "u1001" || !got.GetPublishAudio() || !got.GetPublishVideo() || got.GetClientIceUfrag() != "clientUfrag" {
		t.Fatalf("unexpected setup transport request: %+v", got)
	}

	msgType, payload := readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected media answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	var answer protocol.MediaAnswerBody
	if err := proto.Unmarshal(payload, &answer); err != nil {
		t.Fatalf("unmarshal answer failed: %v", err)
	}
	if answer.GetMeetingId() != meeting.ID || answer.GetServerIceUfrag() != "serverUfrag" || answer.GetAssignedAudioSsrc() != 1111 || answer.GetAssignedVideoSsrc() != 2222 {
		t.Fatalf("unexpected answer payload: %+v", answer)
	}

	msgType, payload = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MeetStateSync {
		t.Fatalf("unexpected state sync type after transport answer: got %v want %v", msgType, protocol.MeetStateSync)
	}
	var sync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(payload, &sync); err != nil {
		t.Fatalf("unmarshal media state sync failed: %v", err)
	}
	assertParticipantMediaSsrc(t, sync.Participants, "u1001", 1111, 2222)

	candidatePayload, err := proto.Marshal(&protocol.MediaIceCandidateBody{
		MeetingId:       meeting.ID,
		Candidate:       "candidate:2 1 udp 2130706431 127.0.0.1 5005 typ host",
		SdpMid:          "audio",
		SdpMlineIndex:   0,
		EndOfCandidates: true,
	})
	if err != nil {
		t.Fatalf("marshal candidate failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaIceCandidate, candidatePayload)
	if len(sfuClient.trickleIceReqs) != 1 {
		t.Fatalf("expected one trickle candidate call, got %d", len(sfuClient.trickleIceReqs))
	}
	if got := sfuClient.trickleIceReqs[0]; got.GetMeetingId() != meeting.ID || got.GetUserId() != "u1001" || got.GetSdpMid() != "audio" || !got.GetEndOfCandidates() {
		t.Fatalf("unexpected trickle request: %+v", got)
	}
}

func TestRouterAllowsReceiveOnlyTransportOffer(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sfuClient := &stubSFUClient{
		setupTransportRsp: &pb.SetupTransportRsp{
			Success:               true,
			ServerIceUfrag:        "serverUfrag",
			ServerIcePwd:          "serverPwd",
			ServerDtlsFingerprint: "AA:BB",
			ServerCandidates:      []string{"candidate:1 1 udp 2130706431 127.0.0.1 5004 typ host"},
			AssignedAudioSsrc:     0,
			AssignedVideoSsrc:     0,
		},
	}
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	meeting, hostParticipant, err := memStore.CreateMeeting("recv-only-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}

	senderSess, senderConn, senderCleanup := newRunningSession(t, 302, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{
		MeetingId:             meeting.ID,
		PublishAudio:          false,
		PublishVideo:          false,
		ClientIceUfrag:        "recvOnlyUfrag",
		ClientIcePwd:          "recvOnlyPwd",
		ClientDtlsFingerprint: "AA:BB",
		ClientCandidates:      []string{"candidate:1 1 udp 2130706431 127.0.0.1 5006 typ host"},
	})
	if err != nil {
		t.Fatalf("marshal receive-only transport offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	if len(sfuClient.setupTransportReqs) != 1 {
		t.Fatalf("expected one setup transport call, got %d", len(sfuClient.setupTransportReqs))
	}
	if got := sfuClient.setupTransportReqs[0]; got.GetMeetingId() != meeting.ID || got.GetUserId() != "u1001" || got.GetPublishAudio() || got.GetPublishVideo() {
		t.Fatalf("unexpected receive-only setup transport request: %+v", got)
	}

	msgType, payload := readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected media answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	var answer protocol.MediaAnswerBody
	if err := proto.Unmarshal(payload, &answer); err != nil {
		t.Fatalf("unmarshal answer failed: %v", err)
	}
	if answer.GetMeetingId() != meeting.ID || answer.GetAssignedAudioSsrc() != 0 || answer.GetAssignedVideoSsrc() != 0 {
		t.Fatalf("unexpected receive-only answer payload: %+v", answer)
	}
}

func TestRouterIgnoresLegacyMediaMessagesWithoutMeetingID(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sfuClient := &stubSFUClient{}
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 401, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u2001")
	senderSess.SetMeetingID("m-legacy")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 402, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u2002")
	targetSess.SetMeetingID("m-legacy")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	answerPayload, err := proto.Marshal(&protocol.MediaAnswerBody{})
	if err != nil {
		t.Fatalf("marshal answer failed: %v", err)
	}
	router.HandleMessage(targetSess, protocol.MediaAnswer, answerPayload)

	candidatePayload, err := proto.Marshal(&protocol.MediaIceCandidateBody{Candidate: "cand", SdpMid: "0", SdpMlineIndex: 1})
	if err != nil {
		t.Fatalf("marshal candidate failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaIceCandidate, candidatePayload)

	if len(sfuClient.setupTransportReqs) != 0 || len(sfuClient.trickleIceReqs) != 0 || len(sfuClient.requests) != 0 || len(sfuClient.addSubscriberReqs) != 0 {
		t.Fatalf("legacy media messages must not touch SFU client: %+v", sfuClient)
	}
	assertNoFrame(t, targetConn, "target on legacy media")
	assertNoFrame(t, senderConn, "sender on legacy media")
}

func TestMediaHandlerRecoveryDoesNotRequireHostMetadata(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
	}

	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	if err := roomState.UpsertRoom(context.Background(), "hostless-room", "", "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("seed room route failed: %v", err)
	}

	primaryRouteClient := &stubSFUClient{addPublisherErr: errors.New("primary node down")}
	backupRouteClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "10.0.0.3:10000"},
	}
	sfuClient := signalingSfu.NewRoutedClientWithClients(&stubSFUClient{}, roomState, map[string]signalingSfu.Client{
		"10.0.0.2:10000": primaryRouteClient,
		"10.0.0.3:10000": backupRouteClient,
	})
	mediaHandler := NewMediaHandler(cfg, nil, nil, roomState, sfuClient, nil, nil)

	session, _, cleanup := newRunningSession(t, 861, cfg)
	defer cleanup()
	session.SetMeetingID("hostless-room")
	session.SetState(server.StateInMeeting)
	session.SetUserID("u1001")

	mediaHandler.registerPublisher(session, 10001, 0)

	if len(primaryRouteClient.requests) == 0 {
		t.Fatal("expected failed add publisher attempts on primary route")
	}
	if len(backupRouteClient.createRoomRequests) != 1 {
		t.Fatalf("expected one recovery create room on backup node, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 1 {
		t.Fatalf("expected one recovery add publisher on backup node, got %d", len(backupRouteClient.requests))
	}

	meta, err := roomState.RoomMetadata(context.Background(), "hostless-room")
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-b" || meta.SFUAddress != "10.0.0.3:10000" {
		t.Fatalf("expected room route switched to backup node, got %+v", meta)
	}
}

func TestMediaHandlerRecoveryConcurrentSwitchRollsBackTempRoomAndRetriesLatestRoute(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
		{NodeID: "sfu-c", MediaAddress: "10.0.0.4:10000", RPCAddress: "10.0.0.4:19000", MaxMeetings: 10},
	}

	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	if err := roomState.UpsertRoom(context.Background(), "concurrent-switch-room", "u1001", "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("seed room route failed: %v", err)
	}

	primaryRouteClient := &stubSFUClient{addPublisherErr: errors.New("primary node down")}
	var switchedOnce bool
	backupRouteClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "10.0.0.3:10000"},
		addPublisherHook: func(_ *pb.AddPublisherReq) {
			if switchedOnce {
				return
			}
			switchedOnce = true
			if _, err := roomState.SwitchRoomRoute(context.Background(), "concurrent-switch-room", "sfu-a", "sfu-c", "10.0.0.4:10000"); err != nil {
				t.Errorf("switch room route from hook failed: %v", err)
			}
		},
	}
	latestRouteClient := &stubSFUClient{}
	sfuClient := signalingSfu.NewRoutedClientWithClients(&stubSFUClient{}, roomState, map[string]signalingSfu.Client{
		"10.0.0.2:10000": primaryRouteClient,
		"10.0.0.3:10000": backupRouteClient,
		"10.0.0.4:10000": latestRouteClient,
	})
	mediaHandler := NewMediaHandler(cfg, nil, nil, roomState, sfuClient, nil, nil)

	session, _, cleanup := newRunningSession(t, 862, cfg)
	defer cleanup()
	session.SetMeetingID("concurrent-switch-room")
	session.SetState(server.StateInMeeting)
	session.SetUserID("u1001")

	mediaHandler.registerPublisher(session, 12001, 0)

	if len(primaryRouteClient.requests) == 0 {
		t.Fatal("expected failed add publisher attempts on primary route")
	}
	if len(backupRouteClient.createRoomRequests) != 1 {
		t.Fatalf("expected one temporary create room on backup node, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 1 {
		t.Fatalf("expected one temporary add publisher on backup node, got %d", len(backupRouteClient.requests))
	}
	if len(backupRouteClient.destroyRoomReqs) != 1 {
		t.Fatalf("expected rollback destroy room on backup node, got %d", len(backupRouteClient.destroyRoomReqs))
	}
	if len(latestRouteClient.requests) != 1 {
		t.Fatalf("expected latest routed client retry once, got %d", len(latestRouteClient.requests))
	}

	meta, err := roomState.RoomMetadata(context.Background(), "concurrent-switch-room")
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-c" || meta.SFUAddress != "10.0.0.4:10000" {
		t.Fatalf("expected room metadata to keep concurrent switched route, got %+v", meta)
	}

	metrics, err := roomState.LoadRecoveryMetrics(context.Background(), cfg.NodeID)
	if err != nil {
		t.Fatalf("load recovery metrics failed: %v", err)
	}
	if metrics == nil {
		t.Fatal("expected recovery metrics")
	}
	if metrics.FailoverAttempts != 1 || metrics.FailoverSuccess != 1 || metrics.FailoverFailed != 0 {
		t.Fatalf("unexpected failover metrics after concurrent switch recovery: %+v", metrics)
	}
}

func TestMediaHandlerRecoverySerializesConcurrentFailover(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
	}

	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()

	meeting, hostParticipant, err := memStore.CreateMeeting("recover-concurrent-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, joined, err := memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("seed room route failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, joined); err != nil {
		t.Fatalf("add joined member failed: %v", err)
	}

	primaryRouteClient := &stubSFUClient{addPublisherErr: errors.New("primary node down")}
	backupRouteClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "10.0.0.3:10000"},
	}
	sfuClient := signalingSfu.NewRoutedClientWithClients(&stubSFUClient{}, roomState, map[string]signalingSfu.Client{
		"10.0.0.2:10000": primaryRouteClient,
		"10.0.0.3:10000": backupRouteClient,
	})
	mediaHandler := NewMediaHandler(cfg, nil, memStore, roomState, sfuClient, nil, nil)

	s1, _, cleanup1 := newRunningSession(t, 801, cfg)
	defer cleanup1()
	s1.SetMeetingID(meeting.ID)
	s1.SetState(server.StateInMeeting)
	s1.SetUserID("u1001")

	s2, _, cleanup2 := newRunningSession(t, 802, cfg)
	defer cleanup2()
	s2.SetMeetingID(meeting.ID)
	s2.SetState(server.StateInMeeting)
	s2.SetUserID("u1002")

	start := make(chan struct{})
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		<-start
		mediaHandler.registerPublisher(s1, 1111, 0)
	}()
	go func() {
		defer wg.Done()
		<-start
		mediaHandler.registerPublisher(s2, 2222, 0)
	}()

	close(start)
	wg.Wait()

	if len(backupRouteClient.createRoomRequests) != 1 {
		t.Fatalf("expected one recovery create room call on backup node, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 2 {
		t.Fatalf("expected both publishers to register on backup node, got %d", len(backupRouteClient.requests))
	}
	if len(primaryRouteClient.requests) < 2 {
		t.Fatalf("expected primary node to observe failed publisher registration attempts, got %d", len(primaryRouteClient.requests))
	}

	meta, err := roomState.RoomMetadata(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-b" || meta.SFUAddress != "10.0.0.3:10000" {
		t.Fatalf("expected room route switched to backup node, got %+v", meta)
	}
	if !mini.Exists("sfu_node_quarantine:sfu-a") {
		t.Fatal("expected failed primary node to be quarantined")
	}
}

func TestMediaHandlerRecoverySkipsWhenDistributedLockHeld(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
	}

	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sessions := server.NewSessionManager()

	meeting, hostParticipant, err := memStore.CreateMeeting("recover-locked-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("seed room route failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}

	mini.Set("sfu_recovery_lock:"+meeting.ID, "sig-node-b:u2001:123")

	primaryRouteClient := &stubSFUClient{addPublisherErr: errors.New("primary node down")}
	backupRouteClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "10.0.0.3:10000"},
	}
	sfuClient := signalingSfu.NewRoutedClientWithClients(&stubSFUClient{}, roomState, map[string]signalingSfu.Client{
		"10.0.0.2:10000": primaryRouteClient,
		"10.0.0.3:10000": backupRouteClient,
	})
	mediaHandler := NewMediaHandler(cfg, sessions, memStore, roomState, sfuClient, nil, nil)

	session, sessionConn, cleanup := newRunningSession(t, 826, cfg)
	defer cleanup()
	sessions.Add(session)
	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)
	session.SetUserID("u1001")

	mediaHandler.registerPublisher(session, 8888, 0)

	if len(primaryRouteClient.requests) == 0 {
		t.Fatal("expected failed add publisher attempts on primary route")
	}
	if len(backupRouteClient.createRoomRequests) != 0 {
		t.Fatalf("expected no backup create room while distributed lock held, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 0 {
		t.Fatalf("expected no backup add publisher while distributed lock held, got %d", len(backupRouteClient.requests))
	}

	meta, err := roomState.RoomMetadata(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-a" || meta.SFUAddress != "10.0.0.2:10000" {
		t.Fatalf("expected room route to remain unchanged while lock held, got %+v", meta)
	}

	sourceStatus, err := roomState.SFUNodeStatus(context.Background(), "sfu-a")
	if err != nil {
		t.Fatalf("load source sfu status failed: %v", err)
	}
	if sourceStatus != nil {
		t.Fatalf("expected no per-sfu recovery metrics while distributed lock held, got %+v", sourceStatus)
	}
	targetStatus, err := roomState.SFUNodeStatus(context.Background(), "sfu-b")
	if err != nil {
		t.Fatalf("load target sfu status failed: %v", err)
	}
	if targetStatus != nil {
		t.Fatalf("expected no target sfu status while distributed lock held, got %+v", targetStatus)
	}

	metrics, err := roomState.LoadRecoveryMetrics(context.Background(), cfg.NodeID)
	if err != nil {
		t.Fatalf("load recovery metrics failed: %v", err)
	}
	if metrics == nil {
		t.Fatal("expected recovery metrics while distributed lock held")
	}
	if metrics.LockAttempts != 1 || metrics.LockAcquired != 0 || metrics.LockContended != 1 {
		t.Fatalf("unexpected lock contention metrics: %+v", metrics)
	}
	if metrics.FollowupAttempts != 1 || metrics.FollowupSuccess != 0 || metrics.FollowupFailed != 1 {
		t.Fatalf("unexpected followup metrics while distributed lock held: %+v", metrics)
	}
	if metrics.FailoverAttempts != 0 || metrics.FailoverSuccess != 0 || metrics.FailoverFailed != 0 {
		t.Fatalf("unexpected failover metrics while distributed lock held: %+v", metrics)
	}

	assertNoFrame(t, sessionConn, "session route status while distributed lock held")
}

func TestMediaHandlerRecoveryRollsBackCreatedRoomOnAddPublisherFailure(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
	}

	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	sessions := server.NewSessionManager()

	meeting, hostParticipant, err := memStore.CreateMeeting("recover-rollback-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-a", "10.0.0.2:10000"); err != nil {
		t.Fatalf("seed room route failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}

	primaryRouteClient := &stubSFUClient{addPublisherErr: errors.New("primary node down")}
	backupRouteClient := &stubSFUClient{
		createRoomResponse: &pb.CreateRoomRsp{Success: true, SfuAddress: "10.0.0.3:10000"},
		addPublisherErr:    errors.New("publisher registration failed"),
		destroyRoomRsp:     &pb.DestroyRoomRsp{Success: true},
	}
	sfuClient := signalingSfu.NewRoutedClientWithClients(&stubSFUClient{}, roomState, map[string]signalingSfu.Client{
		"10.0.0.2:10000": primaryRouteClient,
		"10.0.0.3:10000": backupRouteClient,
	})
	mediaHandler := NewMediaHandler(cfg, sessions, memStore, roomState, sfuClient, nil, nil)

	session, sessionConn, cleanup := newRunningSession(t, 851, cfg)
	defer cleanup()
	sessions.Add(session)
	session.SetMeetingID(meeting.ID)
	session.SetState(server.StateInMeeting)
	session.SetUserID("u1001")

	mediaHandler.registerPublisher(session, 9999, 0)

	if len(backupRouteClient.createRoomRequests) != 1 {
		t.Fatalf("expected one recovery create room on backup node, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 1 {
		t.Fatalf("expected one failed add publisher on backup node, got %d", len(backupRouteClient.requests))
	}
	if len(backupRouteClient.destroyRoomReqs) != 1 {
		t.Fatalf("expected rollback destroy room on backup node, got %d", len(backupRouteClient.destroyRoomReqs))
	}

	meta, err := roomState.RoomMetadata(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-a" || meta.SFUAddress != "10.0.0.2:10000" {
		t.Fatalf("expected room route to remain on original node after rollback, got %+v", meta)
	}
	if !mini.Exists("sfu_node_quarantine:sfu-a") {
		t.Fatal("expected failed primary node to be quarantined")
	}
	if !mini.Exists("sfu_node_quarantine:sfu-b") {
		t.Fatal("expected failed backup node to be quarantined after rollback")
	}

	msgType, payload := readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type after rollback: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	msgType, payload = readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type after rollback: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageFailed)
}

func TestMediaHandlerRouteStatusDeduplicatesWithinWindow(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	mediaHandler := NewMediaHandler(cfg, sessions, nil, nil, nil, nil, nil)

	session, sessionConn, cleanup := newRunningSession(t, 881, cfg)
	defer cleanup()
	sessions.Add(session)
	sessions.BindUser(session, "u1001")
	session.SetMeetingID("m-route-status-dedup")
	session.SetState(server.StateInMeeting)

	switching := mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitching,
		Message:    "Switching SFU media route",
		FromNodeID: "sfu-a",
	}
	mediaHandler.broadcastRouteStatus("m-route-status-dedup", switching)
	mediaHandler.broadcastRouteStatus("m-route-status-dedup", switching)

	msgType, payload := readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)
	assertNoFrame(t, sessionConn, "deduplicated switching route status")

	mediaHandler.broadcastRouteStatus("m-route-status-dedup", mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitched,
		Message:    "SFU media route switched to 10.0.0.3:10000",
		FromNodeID: "sfu-a",
		ToNodeID:   "sfu-b",
		Route:      "10.0.0.3:10000",
	})
	msgType, payload = readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type after stage transition: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitched)
}

func TestMediaHandlerRouteStatusMetricsTrackEmittedAndDeduped(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.NodeID = "sig-node-a"
	cfg.SFURouteStatusDedup = 40 * time.Millisecond

	sessions := server.NewSessionManager()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	mediaHandler := NewMediaHandler(cfg, sessions, nil, roomState, nil, nil, nil)

	session, sessionConn, cleanup := newRunningSession(t, 892, cfg)
	defer cleanup()
	sessions.Add(session)
	sessions.BindUser(session, "u1001")
	session.SetMeetingID("m-route-status-metrics")
	session.SetState(server.StateInMeeting)

	event := mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitching,
		Message:    "Switching SFU media route",
		FromNodeID: "sfu-a",
	}

	mediaHandler.broadcastRouteStatus("m-route-status-metrics", event)
	msgType, payload := readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	mediaHandler.broadcastRouteStatus("m-route-status-metrics", event)
	assertNoFrame(t, sessionConn, "deduplicated switching route status")

	time.Sleep(70 * time.Millisecond)
	mediaHandler.broadcastRouteStatus("m-route-status-metrics", event)
	msgType, payload = readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type after dedup window elapsed: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	metrics, err := roomState.LoadRecoveryMetrics(context.Background(), cfg.NodeID)
	if err != nil {
		t.Fatalf("load route status metrics failed: %v", err)
	}
	if metrics == nil {
		t.Fatal("expected signaling recovery metrics snapshot")
	}
	if metrics.RouteStatusSent != 2 || metrics.RouteStatusDrop != 1 {
		t.Fatalf("unexpected route status metric counts: %+v", metrics)
	}
}

func TestMediaHandlerRouteStatusCacheSweepsStaleMeetings(t *testing.T) {
	cfg := config.Load()
	cfg.SFURouteStatusDedup = 40 * time.Millisecond
	sessions := server.NewSessionManager()
	mediaHandler := NewMediaHandler(cfg, sessions, nil, nil, nil, nil, nil)

	now := time.Now()
	mediaHandler.routeStatusCache = map[string]routeStatusRecord{
		"old-meeting": {
			fingerprint: "switching|sfu-a||",
			emittedAt:   now.Add(-time.Second),
		},
		"fresh-meeting": {
			fingerprint: "switching|sfu-a||",
			emittedAt:   now.Add(-20 * time.Millisecond),
		},
	}
	mediaHandler.routeStatusSweep = now.Add(-2 * time.Minute)

	if !mediaHandler.shouldEmitRouteStatus("trigger-meeting", mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitching,
		Message:    "Switching SFU media route",
		FromNodeID: "sfu-a",
	}) {
		t.Fatal("expected route status to be emitted for trigger meeting")
	}

	if _, ok := mediaHandler.routeStatusCache["old-meeting"]; ok {
		t.Fatalf("expected stale route status cache entry to be swept, got %+v", mediaHandler.routeStatusCache["old-meeting"])
	}
	if _, ok := mediaHandler.routeStatusCache["fresh-meeting"]; !ok {
		t.Fatalf("expected fresh route status cache entry to be retained, got %+v", mediaHandler.routeStatusCache)
	}
	if _, ok := mediaHandler.routeStatusCache["trigger-meeting"]; !ok {
		t.Fatalf("expected trigger meeting to be cached, got %+v", mediaHandler.routeStatusCache)
	}
}

func TestRouterBroadcastsStateSyncForMediaMuteToggle(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, hostParticipant, err := memStore.CreateMeeting("media-state-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, participants, joined, err := memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, joined); err != nil {
		t.Fatalf("add joined member failed: %v", err)
	}
	_ = participants

	senderSess, senderConn, senderCleanup := newRunningSession(t, 601, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 602, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MediaMuteToggleBody{MediaType: 0, Muted: true})
	if err != nil {
		t.Fatalf("marshal media mute toggle failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaMuteToggle, payload)

	senderType, senderPayload := readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if senderType != protocol.MeetStateSync {
		t.Fatalf("unexpected sender sync type: got %v want %v", senderType, protocol.MeetStateSync)
	}
	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetStateSync {
		t.Fatalf("unexpected target sync type: got %v want %v", targetType, protocol.MeetStateSync)
	}

	var senderSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(senderPayload, &senderSync); err != nil {
		t.Fatalf("unmarshal sender state sync failed: %v", err)
	}
	var targetSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(targetPayload, &targetSync); err != nil {
		t.Fatalf("unmarshal target state sync failed: %v", err)
	}

	assertParticipantMediaState(t, senderSync.Participants, "u1001", false, true, false)
	assertParticipantMediaState(t, targetSync.Participants, "u1001", false, true, false)
	assertNoFrame(t, senderConn, "sender raw media mute toggle")
	assertNoFrame(t, targetConn, "target raw media mute toggle")
}

func TestRouterBroadcastsStateSyncForMediaMuteToggleWithoutRedis(t *testing.T) {
	cfg := config.Load()
	cfg.EnableRedis = false

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, _, err := memStore.CreateMeeting("media-state-memory-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	if _, _, _, err := memStore.JoinMeeting(meeting.ID, "", "u1002"); err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}

	senderSess, senderConn, senderCleanup := newRunningSession(t, 611, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 612, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MediaMuteToggleBody{MediaType: 1, Muted: true})
	if err != nil {
		t.Fatalf("marshal media mute toggle failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaMuteToggle, payload)

	senderType, senderPayload := readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if senderType != protocol.MeetStateSync {
		t.Fatalf("unexpected sender sync type: got %v want %v", senderType, protocol.MeetStateSync)
	}
	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetStateSync {
		t.Fatalf("unexpected target sync type: got %v want %v", targetType, protocol.MeetStateSync)
	}

	var senderSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(senderPayload, &senderSync); err != nil {
		t.Fatalf("unmarshal sender state sync failed: %v", err)
	}
	var targetSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(targetPayload, &targetSync); err != nil {
		t.Fatalf("unmarshal target state sync failed: %v", err)
	}

	assertParticipantMediaState(t, senderSync.Participants, "u1001", true, false, false)
	assertParticipantMediaState(t, targetSync.Participants, "u1001", true, false, false)
	assertNoFrame(t, senderConn, "sender raw media mute toggle without redis")
	assertNoFrame(t, targetConn, "target raw media mute toggle without redis")
}

func TestRouterBroadcastsStateSyncForMediaScreenShare(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	meeting, hostParticipant, err := memStore.CreateMeeting("screen-share-room", "", "u1001", 4)
	if err != nil {
		t.Fatalf("create meeting failed: %v", err)
	}
	_, _, joined, err := memStore.JoinMeeting(meeting.ID, "", "u1002")
	if err != nil {
		t.Fatalf("join meeting failed: %v", err)
	}
	if err := roomState.UpsertRoom(context.Background(), meeting.ID, hostParticipant.UserId, "sfu-node-01", "127.0.0.1:5000"); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, hostParticipant); err != nil {
		t.Fatalf("add host member failed: %v", err)
	}
	if err := roomState.AddMember(context.Background(), meeting.ID, joined); err != nil {
		t.Fatalf("add joined member failed: %v", err)
	}

	senderSess, senderConn, senderCleanup := newRunningSession(t, 701, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 702, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	payload, err := proto.Marshal(&protocol.MediaScreenShareBody{Sharing: true})
	if err != nil {
		t.Fatalf("marshal media screen share failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaScreenShare, payload)

	senderType, senderPayload := readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if senderType != protocol.MeetStateSync {
		t.Fatalf("unexpected sender sync type: got %v want %v", senderType, protocol.MeetStateSync)
	}
	targetType, targetPayload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if targetType != protocol.MeetStateSync {
		t.Fatalf("unexpected target sync type: got %v want %v", targetType, protocol.MeetStateSync)
	}

	var senderSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(senderPayload, &senderSync); err != nil {
		t.Fatalf("unmarshal sender screen share sync failed: %v", err)
	}
	var targetSync protocol.MeetStateSyncNotifyBody
	if err := proto.Unmarshal(targetPayload, &targetSync); err != nil {
		t.Fatalf("unmarshal target screen share sync failed: %v", err)
	}

	assertParticipantMediaState(t, senderSync.Participants, "u1001", true, true, true)
	assertParticipantMediaState(t, targetSync.Participants, "u1001", true, true, true)
	assertNoFrame(t, senderConn, "sender raw media screen share")
	assertNoFrame(t, targetConn, "target raw media screen share")
}

func decodeFrameBytes(t *testing.T, frame []byte, maxPayload uint32) (protocol.SignalType, []byte) {
	t.Helper()

	if len(frame) < protocol.HeaderSize {
		t.Fatalf("frame too short: %d", len(frame))
	}

	msgType, payloadLen, err := protocol.DecodeHeader(frame[:protocol.HeaderSize], maxPayload)
	if err != nil {
		t.Fatalf("decode frame header failed: %v", err)
	}
	if int(protocol.HeaderSize+payloadLen) != len(frame) {
		t.Fatalf("unexpected frame size: got %d want %d", len(frame), protocol.HeaderSize+payloadLen)
	}

	payload := append([]byte(nil), frame[protocol.HeaderSize:]...)
	return msgType, payload
}

type routeStatusEventPayload struct {
	Stage      string `json:"stage"`
	Message    string `json:"message"`
	FromNodeID string `json:"from_node_id"`
	ToNodeID   string `json:"to_node_id"`
	Route      string `json:"route"`
}

func decodeRouteStatusEvent(t *testing.T, payload []byte) routeStatusEventPayload {
	t.Helper()

	var notify protocol.MediaRouteStatusNotifyBody
	if err := proto.Unmarshal(payload, &notify); err != nil {
		t.Fatalf("unmarshal MediaRouteStatusNotify failed: %v", err)
	}

	reason := strings.TrimSpace(notify.GetReason())
	if reason == "" {
		return routeStatusEventPayload{}
	}

	var event routeStatusEventPayload
	if err := json.Unmarshal([]byte(reason), &event); err == nil {
		event.Stage = strings.ToLower(strings.TrimSpace(event.Stage))
		event.Message = strings.TrimSpace(event.Message)
		if event.Message == "" {
			event.Message = reason
		}
		return event
	}

	event.Message = reason
	lower := strings.ToLower(reason)
	switch {
	case strings.Contains(lower, "switching"):
		event.Stage = routeStatusStageSwitching
	case strings.Contains(lower, "failed"):
		event.Stage = routeStatusStageFailed
	default:
		event.Stage = routeStatusStageSwitched
	}
	return event
}

func assertRouteStatusStage(t *testing.T, payload []byte, expectedStage string) routeStatusEventPayload {
	t.Helper()

	event := decodeRouteStatusEvent(t, payload)
	if event.Stage != expectedStage {
		t.Fatalf("unexpected route status stage: got %q want %q (message=%q)", event.Stage, expectedStage, event.Message)
	}
	if event.Message == "" {
		t.Fatalf("expected non-empty route status message for stage %q", expectedStage)
	}
	return event
}

func assertParticipantMediaState(t *testing.T, participants []*protocol.Participant, userID string, audioOn, videoOn, sharing bool) {
	t.Helper()

	for _, participant := range participants {
		if participant == nil || participant.UserId != userID {
			continue
		}
		if participant.IsAudioOn != audioOn || participant.IsVideoOn != videoOn || participant.IsSharing != sharing {
			t.Fatalf("unexpected participant media state for %s: %+v", userID, participant)
		}
		return
	}

	t.Fatalf("participant %s not found in state sync", userID)
}

func assertParticipantMediaSsrc(t *testing.T, participants []*protocol.Participant, userID string, audioSsrc, videoSsrc uint32) {
	t.Helper()

	for _, participant := range participants {
		if participant == nil || participant.UserId != userID {
			continue
		}
		if participant.AudioSsrc != audioSsrc || participant.VideoSsrc != videoSsrc {
			t.Fatalf("unexpected participant SSRCs for %s: got audio=%d video=%d want audio=%d video=%d",
				userID,
				participant.AudioSsrc,
				participant.VideoSsrc,
				audioSsrc,
				videoSsrc)
		}
		return
	}

	t.Fatalf("participant %s not found in state sync", userID)
}

func TestMediaHandlerRouteStatusDedupWindowConfigurable(t *testing.T) {
	cfg := config.Load()
	cfg.SFURouteStatusDedup = 40 * time.Millisecond
	sessions := server.NewSessionManager()
	mediaHandler := NewMediaHandler(cfg, sessions, nil, nil, nil, nil, nil)

	session, sessionConn, cleanup := newRunningSession(t, 891, cfg)
	defer cleanup()
	sessions.Add(session)
	sessions.BindUser(session, "u1001")
	session.SetMeetingID("m-route-status-dedup-config")
	session.SetState(server.StateInMeeting)

	event := mediaRouteStatusEvent{
		Stage:      routeStatusStageSwitching,
		Message:    "Switching SFU media route",
		FromNodeID: "sfu-a",
	}

	mediaHandler.broadcastRouteStatus("m-route-status-dedup-config", event)
	msgType, payload := readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	mediaHandler.broadcastRouteStatus("m-route-status-dedup-config", event)
	assertNoFrame(t, sessionConn, "deduplicated switching route status within configured window")

	time.Sleep(70 * time.Millisecond)
	mediaHandler.broadcastRouteStatus("m-route-status-dedup-config", event)
	msgType, payload = readFrame(t, sessionConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected route status type after dedup window elapsed: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)
}
