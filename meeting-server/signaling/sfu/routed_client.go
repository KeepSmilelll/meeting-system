package sfu

import (
	"context"
	"strings"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol/pb"
	"meeting-server/signaling/store"
)

type routedClient struct {
	defaultClient Client
	roomState     *store.RedisRoomStore
	routeClients  map[string]Client
}

func NewRoutedClient(cfg config.Config, roomState *store.RedisRoomStore, options ...Option) Client {
	defaultClient := NewClient(cfg.SFURPCAddr, options...)
	routeClients := make(map[string]Client)
	for _, node := range cfg.EffectiveSFUNodes() {
		route := strings.TrimSpace(node.MediaAddress)
		rpcAddr := strings.TrimSpace(node.RPCAddress)
		if route == "" || rpcAddr == "" {
			continue
		}
		routeClients[route] = NewClient(rpcAddr, options...)
		if route == strings.TrimSpace(cfg.DefaultSFUAddress) {
			defaultClient = routeClients[route]
		}
	}
	for route, rpcAddr := range cfg.SFURouteRPCMap {
		trimmedRoute := strings.TrimSpace(route)
		trimmedRPC := strings.TrimSpace(rpcAddr)
		if trimmedRoute == "" || trimmedRPC == "" {
			continue
		}
		if _, ok := routeClients[trimmedRoute]; !ok {
			routeClients[trimmedRoute] = NewClient(trimmedRPC, options...)
		}
	}
	defaultRoute := strings.TrimSpace(cfg.DefaultSFUAddress)
	if defaultRoute != "" {
		if existing, ok := routeClients[defaultRoute]; ok && cfg.SFURPCAddr == "" {
			defaultClient = existing
		}
		if _, ok := routeClients[defaultRoute]; !ok {
			routeClients[defaultRoute] = defaultClient
		}
	}
	return NewRoutedClientWithClients(defaultClient, roomState, routeClients)
}

type routeAwareClient interface {
	clientForRoute(route string) Client
}

func NewRoutedClientWithClients(defaultClient Client, roomState *store.RedisRoomStore, routeClients map[string]Client) Client {
	if defaultClient == nil {
		defaultClient = NewDisabledClient()
	}

	normalized := make(map[string]Client, len(routeClients))
	for route, client := range routeClients {
		trimmed := strings.TrimSpace(route)
		if trimmed == "" || client == nil {
			continue
		}
		normalized[trimmed] = client
	}

	return &routedClient{
		defaultClient: defaultClient,
		roomState:     roomState,
		routeClients:  normalized,
	}
}

func (c *routedClient) CreateRoom(ctx context.Context, req *pb.CreateRoomReq) (*pb.CreateRoomRsp, error) {
	return c.defaultClientForRoute().CreateRoom(ctx, req)
}

func (c *routedClient) DestroyRoom(ctx context.Context, req *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).DestroyRoom(ctx, req)
}

func (c *routedClient) SetupTransport(ctx context.Context, req *pb.SetupTransportReq) (*pb.SetupTransportRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).SetupTransport(ctx, req)
}

func (c *routedClient) TrickleIceCandidate(ctx context.Context, req *pb.TrickleIceCandidateReq) (*pb.TrickleIceCandidateRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).TrickleIceCandidate(ctx, req)
}

func (c *routedClient) CloseTransport(ctx context.Context, req *pb.CloseTransportReq) (*pb.CloseTransportRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).CloseTransport(ctx, req)
}

func (c *routedClient) AddPublisher(ctx context.Context, req *pb.AddPublisherReq) (*pb.AddPublisherRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).AddPublisher(ctx, req)
}

func (c *routedClient) AddSubscriber(ctx context.Context, req *pb.AddSubscriberReq) (*pb.AddSubscriberRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).AddSubscriber(ctx, req)
}

func (c *routedClient) RemovePublisher(ctx context.Context, req *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).RemovePublisher(ctx, req)
}

func (c *routedClient) RemoveSubscriber(ctx context.Context, req *pb.RemoveSubscriberReq) (*pb.RemoveSubscriberRsp, error) {
	return c.clientForMeeting(ctx, req.GetMeetingId()).RemoveSubscriber(ctx, req)
}

func (c *routedClient) GetNodeStatus(ctx context.Context, req *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error) {
	return c.defaultClientForRoute().GetNodeStatus(ctx, req)
}

func (c *routedClient) clientForMeeting(ctx context.Context, meetingID string) Client {
	if c == nil {
		return NewDisabledClient()
	}
	if c.roomState != nil && meetingID != "" {
		meta, err := c.roomState.RoomMetadata(ctx, meetingID)
		if err == nil && meta != nil {
			if client := c.clientForRoute(meta.SFUAddress); client != nil {
				return client
			}
		}
	}
	return c.defaultClientForRoute()
}

func (c *routedClient) clientForRoute(route string) Client {
	if c == nil {
		return nil
	}
	trimmedRoute := strings.TrimSpace(route)
	if client, ok := c.routeClients[trimmedRoute]; ok && client != nil {
		return client
	}
	if c.roomState != nil && trimmedRoute != "" {
		rpcAddr, err := c.roomState.RPCAddressForRoute(context.Background(), trimmedRoute)
		if err == nil && strings.TrimSpace(rpcAddr) != "" {
			return NewClient(rpcAddr)
		}
	}
	return c.defaultClientForRoute()
}

func (c *routedClient) defaultClientForRoute() Client {
	if c == nil || c.defaultClient == nil {
		return NewDisabledClient()
	}
	return c.defaultClient
}

func ClientForRoute(client Client, route string) Client {
	if aware, ok := client.(routeAwareClient); ok {
		return aware.clientForRoute(route)
	}
	if client == nil {
		return NewDisabledClient()
	}
	return client
}
