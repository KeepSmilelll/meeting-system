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

	createRoomRequests  []*pb.CreateRoomReq
	createRoomResponse  *pb.CreateRoomRsp
	createRoomErr       error
	requests            []*pb.AddPublisherReq
	removePublisherReqs []*pb.RemovePublisherReq
	destroyRoomReqs     []*pb.DestroyRoomReq
	addPublisherRsp     *pb.AddPublisherRsp
	addPublisherErr     error
	addPublisherHook    func(*pb.AddPublisherReq)
	removePublisherRsp  *pb.RemovePublisherRsp
	removePublisherErr  error
	destroyRoomRsp      *pb.DestroyRoomRsp
	destroyRoomErr      error
	getNodeStatusRsp    *pb.GetNodeStatusRsp
	getNodeStatusErr    error
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
	return &pb.AddPublisherRsp{Success: true, UdpPort: 5004}, nil
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

func TestRouterRoutesMediaOfferAnswerAndIceCandidate(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 201, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("m-media")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 202, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-media")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u1002", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, payload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	var offer protocol.MediaOfferBody
	if err := proto.Unmarshal(payload, &offer); err != nil {
		t.Fatalf("unmarshal offer failed: %v", err)
	}
	if offer.TargetUserId != "u1002" || offer.Sdp != "offer-sdp" {
		t.Fatalf("unexpected offer payload: %+v", offer)
	}
	assertNoFrame(t, senderConn, "sender on offer")

	answerPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1001", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal answer failed: %v", err)
	}
	router.HandleMessage(targetSess, protocol.MediaAnswer, answerPayload)

	msgType, payload = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected media answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	var answer protocol.MediaAnswerBody
	if err := proto.Unmarshal(payload, &answer); err != nil {
		t.Fatalf("unmarshal answer failed: %v", err)
	}
	if answer.TargetUserId != "u1001" || answer.Sdp != "answer-sdp" {
		t.Fatalf("unexpected answer payload: %+v", answer)
	}
	assertNoFrame(t, targetConn, "target on answer")

	candidatePayload, err := proto.Marshal(&protocol.MediaIceCandidateBody{TargetUserId: "u1002", Candidate: "cand", SdpMid: "0", SdpMlineIndex: 1})
	if err != nil {
		t.Fatalf("marshal candidate failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaIceCandidate, candidatePayload)

	msgType, payload = readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaIceCandidate {
		t.Fatalf("unexpected media candidate type: got %v want %v", msgType, protocol.MediaIceCandidate)
	}
	var candidate protocol.MediaIceCandidateBody
	if err := proto.Unmarshal(payload, &candidate); err != nil {
		t.Fatalf("unmarshal candidate failed: %v", err)
	}
	if candidate.TargetUserId != "u1002" || candidate.Candidate != "cand" || candidate.SdpMid != "0" || candidate.SdpMlineIndex != 1 {
		t.Fatalf("unexpected candidate payload: %+v", candidate)
	}
}

func TestRouterRejectsInvalidMediaTargets(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, nil, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 301, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID("m-a")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 302, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID("m-b")
	targetSess.SetState(server.StateInMeeting)

	missingTargetPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal missing target payload failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, missingTargetPayload)
	assertNoFrame(t, targetConn, "target on missing target id")
	assertNoFrame(t, senderConn, "sender on missing target id")

	wrongMeetingPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u1002", Sdp: "answer-sdp"})
	if err != nil {
		t.Fatalf("marshal wrong meeting payload failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaAnswer, wrongMeetingPayload)
	assertNoFrame(t, targetConn, "target on wrong meeting")
	assertNoFrame(t, senderConn, "sender on wrong meeting")
}

func TestRouterRegistersPublisherFromMediaDescription(t *testing.T) {
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
	senderSess.SetMeetingID("m-ssrc")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 402, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u2002")
	targetSess.SetMeetingID("m-ssrc")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u2002", Sdp: `{"audio_ssrc":12345}`})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, _ := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	if len(sfuClient.requests) != 1 {
		t.Fatalf("unexpected add publisher call count after offer: got %d want 1", len(sfuClient.requests))
	}
	if got := sfuClient.requests[0]; got.MeetingId != "m-ssrc" || got.UserId != "u2001" || got.AudioSsrc != 12345 {
		t.Fatalf("unexpected add publisher request after offer: %+v", got)
	}
	assertNoFrame(t, senderConn, "sender on offer registration")

	answerPayload, err := proto.Marshal(&protocol.MediaAnswerBody{TargetUserId: "u2001", Sdp: `{"audio_ssrc":54321}`})
	if err != nil {
		t.Fatalf("marshal answer failed: %v", err)
	}
	router.HandleMessage(targetSess, protocol.MediaAnswer, answerPayload)

	msgType, _ = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaAnswer {
		t.Fatalf("unexpected media answer type: got %v want %v", msgType, protocol.MediaAnswer)
	}
	if len(sfuClient.requests) != 2 {
		t.Fatalf("unexpected add publisher call count after answer: got %d want 2", len(sfuClient.requests))
	}
	if got := sfuClient.requests[1]; got.MeetingId != "m-ssrc" || got.UserId != "u2002" || got.AudioSsrc != 54321 {
		t.Fatalf("unexpected add publisher request after answer: %+v", got)
	}
}
func TestRouterRegistersPublisherFromExplicitSsrcFields(t *testing.T) {
	cfg := config.Load()
	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sfuClient := &stubSFUClient{}
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 451, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u3001")
	senderSess.SetMeetingID("m-explicit")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 452, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u3002")
	targetSess.SetMeetingID("m-explicit")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u3002", Sdp: "offer-sdp", AudioSsrc: 111, VideoSsrc: 222})
	if err != nil {
		t.Fatalf("marshal offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, payload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	var offer protocol.MediaOfferBody
	if err := proto.Unmarshal(payload, &offer); err != nil {
		t.Fatalf("unmarshal offer failed: %v", err)
	}
	if offer.GetAudioSsrc() != 111 || offer.GetVideoSsrc() != 222 {
		t.Fatalf("unexpected offer SSRC fields: %+v", offer)
	}
	if len(sfuClient.requests) != 1 {
		t.Fatalf("unexpected add publisher call count after explicit offer: got %d want 1", len(sfuClient.requests))
	}
	if got := sfuClient.requests[0]; got.MeetingId != "m-explicit" || got.UserId != "u3001" || got.AudioSsrc != 111 || got.VideoSsrc != 222 {
		t.Fatalf("unexpected add publisher request after explicit offer: %+v", got)
	}
	assertNoFrame(t, senderConn, "sender on explicit offer registration")
}

func TestRouterRegistersPublisherAgainstRoomRouteClient(t *testing.T) {
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

	const routeAddress = "10.0.0.2:10000"
	if err := roomState.UpsertRoom(context.Background(), "m-routed", "u3001", "sfu-node-02", routeAddress); err != nil {
		t.Fatalf("upsert room failed: %v", err)
	}

	defaultClient := &stubSFUClient{}
	routeClient := &stubSFUClient{}
	sfuClient := signalingSfu.NewRoutedClientWithClients(defaultClient, roomState, map[string]signalingSfu.Client{
		routeAddress: routeClient,
	})
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 461, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u3001")
	senderSess.SetMeetingID("m-routed")
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 462, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u3002")
	targetSess.SetMeetingID("m-routed")
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u3002", Sdp: `{"audio_ssrc":333}`})
	if err != nil {
		t.Fatalf("marshal routed offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, _ := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected routed media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}
	if len(defaultClient.requests) != 0 {
		t.Fatalf("expected default client to stay idle, got %+v", defaultClient.requests)
	}
	if len(routeClient.requests) != 1 {
		t.Fatalf("expected room-route client to receive one add publisher call, got %d", len(routeClient.requests))
	}
	if got := routeClient.requests[0]; got.MeetingId != "m-routed" || got.UserId != "u3001" || got.AudioSsrc != 333 {
		t.Fatalf("unexpected route client add publisher request: %+v", got)
	}
	assertNoFrame(t, senderConn, "sender on routed offer registration")
}

func TestRouterRecoversPublisherRegistrationByReroutingSFUNode(t *testing.T) {
	mini := miniredis.RunT(t)

	cfg := config.Load()
	cfg.EnableRedis = true
	cfg.RedisAddr = mini.Addr()
	cfg.SFUNodeQuarantine = time.Minute
	cfg.SFUNodes = []config.SFUNode{
		{NodeID: "sfu-a", MediaAddress: "10.0.0.2:10000", RPCAddress: "10.0.0.2:19000", MaxMeetings: 10},
		{NodeID: "sfu-b", MediaAddress: "10.0.0.3:10000", RPCAddress: "10.0.0.3:19000", MaxMeetings: 10},
	}

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)

	meeting, hostParticipant, err := memStore.CreateMeeting("reroute-room", "", "u1001", 4)
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
	if err := roomState.SetMuteAll(context.Background(), meeting.ID, true); err != nil {
		t.Fatalf("set mute all failed: %v", err)
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
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, nil, tokenManager, limiter, sfuClient, nil)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 551, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u1001")
	senderSess.SetMeetingID(meeting.ID)
	senderSess.SetState(server.StateInMeeting)

	targetSess, targetConn, targetCleanup := newRunningSession(t, 552, cfg)
	defer targetCleanup()
	sessions.Add(targetSess)
	sessions.BindUser(targetSess, "u1002")
	targetSess.SetMeetingID(meeting.ID)
	targetSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u1002", Sdp: `{"audio_ssrc":12345}`})
	if err != nil {
		t.Fatalf("marshal routed offer failed: %v", err)
	}
	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	msgType, payload := readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected target route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	msgType, payload = readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected target route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitched)

	msgType, _ = readFrame(t, targetConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected media offer type: got %v want %v", msgType, protocol.MediaOffer)
	}

	msgType, payload = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected sender route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitching)

	msgType, payload = readFrame(t, senderConn, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaRouteStatusNotify {
		t.Fatalf("unexpected sender route status type: got %v want %v", msgType, protocol.MediaRouteStatusNotify)
	}
	assertRouteStatusStage(t, payload, routeStatusStageSwitched)

	if len(primaryRouteClient.requests) < 1 {
		t.Fatalf("expected failed add publisher call on primary route, got %d", len(primaryRouteClient.requests))
	}
	if len(backupRouteClient.createRoomRequests) != 1 {
		t.Fatalf("expected recovery create room on backup node, got %d", len(backupRouteClient.createRoomRequests))
	}
	if len(backupRouteClient.requests) != 1 {
		t.Fatalf("expected recovery add publisher on backup node, got %d", len(backupRouteClient.requests))
	}

	meta, err := roomState.RoomMetadata(context.Background(), meeting.ID)
	if err != nil {
		t.Fatalf("load room metadata failed: %v", err)
	}
	if meta == nil || meta.SFUNodeID != "sfu-b" || meta.SFUAddress != "10.0.0.3:10000" || !meta.MuteAll {
		t.Fatalf("expected room route switched to backup node, got %+v", meta)
	}
	if !mini.Exists("sfu_node_quarantine:sfu-a") {
		t.Fatal("expected failed primary node to be quarantined")
	}

	sourceStatus, err := roomState.SFUNodeStatus(context.Background(), "sfu-a")
	if err != nil {
		t.Fatalf("load source sfu status failed: %v", err)
	}
	if sourceStatus == nil || sourceStatus.RecoveryAttempts != 1 || sourceStatus.RecoveryFailoverSuccess != 0 || sourceStatus.RecoveryFailoverFailed != 0 {
		t.Fatalf("unexpected source recovery status: %+v", sourceStatus)
	}
	targetStatus, err := roomState.SFUNodeStatus(context.Background(), "sfu-b")
	if err != nil {
		t.Fatalf("load target sfu status failed: %v", err)
	}
	if targetStatus == nil || targetStatus.RecoveryFailoverSuccess != 1 || targetStatus.RecoveryFailoverFailed != 0 {
		t.Fatalf("unexpected target recovery status: %+v", targetStatus)
	}

	metrics, err := roomState.LoadRecoveryMetrics(context.Background(), cfg.NodeID)
	if err != nil {
		t.Fatalf("load recovery metrics failed: %v", err)
	}
	if metrics == nil {
		t.Fatal("expected recovery metrics for local signaling node")
	}
	if metrics.LockAttempts != 1 || metrics.LockAcquired != 1 || metrics.LockContended != 0 {
		t.Fatalf("unexpected lock metrics: %+v", metrics)
	}
	if metrics.FailoverAttempts != 1 || metrics.FailoverSuccess != 1 || metrics.FailoverFailed != 0 {
		t.Fatalf("unexpected failover metrics on successful reroute: %+v", metrics)
	}

	assertNoFrame(t, senderConn, "sender on rerouted offer registration")
	assertNoFrame(t, targetConn, "target on rerouted offer registration")
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

func TestRouterRoutesRemoteMediaViaNodeBus(t *testing.T) {
	cfg := config.Load()
	cfg.NodeID = "sig-node-a"

	sessions := server.NewSessionManager()
	memStore := store.NewMemoryStore()
	roomState := store.NewRedisRoomStore(cfg)
	defer roomState.Close()
	tokenManager := auth.NewTokenManager("unit-test-secret", time.Hour)
	limiter := auth.NewRateLimiter(nil, 5, 10*time.Minute)
	sessionStore := &stubSessionStore{
		presence: &store.SessionPresence{
			UserID:    "u4002",
			NodeID:    "sig-node-b",
			SessionID: 902,
			MeetingID: "m-remote",
			Status:    int32(server.StateInMeeting),
		},
	}
	directBus := &stubUserFramePublisher{}
	router := NewRouter(cfg, sessions, memStore, memStore, roomState, sessionStore, tokenManager, limiter, nil, directBus)

	senderSess, senderConn, senderCleanup := newRunningSession(t, 501, cfg)
	defer senderCleanup()
	sessions.Add(senderSess)
	sessions.BindUser(senderSess, "u4001")
	senderSess.SetMeetingID("m-remote")
	senderSess.SetState(server.StateInMeeting)

	offerPayload, err := proto.Marshal(&protocol.MediaOfferBody{TargetUserId: "u4002", Sdp: "offer-sdp"})
	if err != nil {
		t.Fatalf("marshal remote offer failed: %v", err)
	}

	router.HandleMessage(senderSess, protocol.MediaOffer, offerPayload)

	if directBus.calls != 1 {
		t.Fatalf("expected one remote publish call, got %d", directBus.calls)
	}
	if directBus.nodeID != "sig-node-b" || directBus.userID != "u4002" || directBus.targetSessionID != 902 {
		t.Fatalf("unexpected remote publish metadata: %+v", directBus)
	}

	msgType, payload := decodeFrameBytes(t, directBus.frame, cfg.MaxPayloadBytes)
	if msgType != protocol.MediaOffer {
		t.Fatalf("unexpected remote media type: got %v want %v", msgType, protocol.MediaOffer)
	}
	var offer protocol.MediaOfferBody
	if err := proto.Unmarshal(payload, &offer); err != nil {
		t.Fatalf("unmarshal remote offer failed: %v", err)
	}
	if offer.TargetUserId != "u4002" || offer.Sdp != "offer-sdp" {
		t.Fatalf("unexpected remote offer payload: %+v", offer)
	}

	assertNoFrame(t, senderConn, "sender on remote offer")
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
