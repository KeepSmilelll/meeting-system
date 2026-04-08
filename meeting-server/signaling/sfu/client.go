package sfu

import (
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"time"

	"meeting-server/signaling/protocol/pb"

	"google.golang.org/protobuf/proto"
)

const (
	wireMagic0 byte = 'S'
	wireMagic1 byte = 'F'
	wireMagic2 byte = 'U'
	wireMagic3 byte = 'R'

	wireVersion byte = 1

	wireKindRequest  byte = 1
	wireKindResponse byte = 2

	wireHeaderSize = 14
	wireMaxPayload = 1 << 20

	defaultTimeout = 5 * time.Second
)

var (
	ErrDisabled         = errors.New("sfu rpc client disabled")
	ErrInvalidRequest   = errors.New("sfu rpc invalid request")
	ErrInvalidResponse  = errors.New("sfu rpc invalid response")
	ErrUnexpectedMethod = errors.New("sfu rpc unexpected method")
	ErrRemoteStatus     = errors.New("sfu rpc remote status")
	ErrFrameTooShort    = errors.New("sfu rpc frame too short")
	ErrBadMagic         = errors.New("sfu rpc bad magic")
	ErrBadVersion       = errors.New("sfu rpc unsupported version")
	ErrBadKind          = errors.New("sfu rpc unexpected frame kind")
	ErrPayloadTooLarge  = errors.New("sfu rpc payload too large")
)

type Client interface {
	CreateRoom(ctx context.Context, req *pb.CreateRoomReq) (*pb.CreateRoomRsp, error)
	DestroyRoom(ctx context.Context, req *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error)
	AddPublisher(ctx context.Context, req *pb.AddPublisherReq) (*pb.AddPublisherRsp, error)
	RemovePublisher(ctx context.Context, req *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error)
	GetNodeStatus(ctx context.Context, req *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error)
}

type Option func(*clientOptions)

type clientOptions struct {
	timeout time.Duration
	dialer  func(ctx context.Context, network, address string) (net.Conn, error)
}

func WithTimeout(timeout time.Duration) Option {
	return func(opts *clientOptions) {
		if timeout > 0 {
			opts.timeout = timeout
		}
	}
}

func WithDialer(dialer func(ctx context.Context, network, address string) (net.Conn, error)) Option {
	return func(opts *clientOptions) {
		if dialer != nil {
			opts.dialer = dialer
		}
	}
}

func NewClient(address string, options ...Option) Client {
	if address == "" {
		return NewDisabledClient()
	}

	opts := clientOptions{
		timeout: defaultTimeout,
		dialer:  (&net.Dialer{}).DialContext,
	}
	for _, option := range options {
		option(&opts)
	}

	return &tcpClient{
		address: address,
		opts:    opts,
	}
}

func NewDisabledClient() Client {
	return disabledClient{}
}

type disabledClient struct{}

func (disabledClient) CreateRoom(context.Context, *pb.CreateRoomReq) (*pb.CreateRoomRsp, error) {
	return &pb.CreateRoomRsp{Success: false}, ErrDisabled
}

func (disabledClient) DestroyRoom(context.Context, *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error) {
	return &pb.DestroyRoomRsp{Success: false}, ErrDisabled
}

func (disabledClient) AddPublisher(context.Context, *pb.AddPublisherReq) (*pb.AddPublisherRsp, error) {
	return &pb.AddPublisherRsp{Success: false}, ErrDisabled
}

func (disabledClient) RemovePublisher(context.Context, *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error) {
	return &pb.RemovePublisherRsp{Success: false}, ErrDisabled
}

func (disabledClient) GetNodeStatus(context.Context, *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error) {
	return &pb.GetNodeStatusRsp{Success: false}, ErrDisabled
}

type tcpClient struct {
	address string
	opts    clientOptions
}

type rpcMethod uint16

const (
	methodCreateRoom       rpcMethod = 1
	methodDestroyRoom      rpcMethod = 2
	methodAddPublisher     rpcMethod = 3
	methodRemovePublisher  rpcMethod = 4
	methodGetNodeStatus    rpcMethod = 5
	methodReportNodeStatus rpcMethod = 6
	methodQualityReport    rpcMethod = 7
)

type wireHeader struct {
	method rpcMethod
	kind   byte
	status uint16
	length uint32
}

func (c *tcpClient) CreateRoom(ctx context.Context, req *pb.CreateRoomReq) (*pb.CreateRoomRsp, error) {
	rsp := &pb.CreateRoomRsp{}
	if err := c.call(ctx, methodCreateRoom, req, rsp); err != nil {
		return rsp, err
	}
	return rsp, nil
}

func (c *tcpClient) DestroyRoom(ctx context.Context, req *pb.DestroyRoomReq) (*pb.DestroyRoomRsp, error) {
	rsp := &pb.DestroyRoomRsp{}
	if err := c.call(ctx, methodDestroyRoom, req, rsp); err != nil {
		return rsp, err
	}
	return rsp, nil
}

func (c *tcpClient) AddPublisher(ctx context.Context, req *pb.AddPublisherReq) (*pb.AddPublisherRsp, error) {
	rsp := &pb.AddPublisherRsp{}
	if err := c.call(ctx, methodAddPublisher, req, rsp); err != nil {
		return rsp, err
	}
	return rsp, nil
}

func (c *tcpClient) RemovePublisher(ctx context.Context, req *pb.RemovePublisherReq) (*pb.RemovePublisherRsp, error) {
	rsp := &pb.RemovePublisherRsp{}
	if err := c.call(ctx, methodRemovePublisher, req, rsp); err != nil {
		return rsp, err
	}
	return rsp, nil
}

func (c *tcpClient) GetNodeStatus(ctx context.Context, req *pb.GetNodeStatusReq) (*pb.GetNodeStatusRsp, error) {
	rsp := &pb.GetNodeStatusRsp{}
	if err := c.call(ctx, methodGetNodeStatus, req, rsp); err != nil {
		return rsp, err
	}
	return rsp, nil
}

func (c *tcpClient) call(ctx context.Context, method rpcMethod, req proto.Message, rsp proto.Message) error {
	if c == nil {
		return ErrDisabled
	}
	if req == nil || rsp == nil {
		return ErrInvalidRequest
	}

	conn, err := c.opts.dialer(ctx, "tcp", c.address)
	if err != nil {
		return fmt.Errorf("sfu rpc: dial %s: %w", c.address, err)
	}
	defer conn.Close()

	deadline, ok := requestDeadline(ctx, c.opts.timeout)
	if ok {
		if err := conn.SetDeadline(deadline); err != nil {
			return fmt.Errorf("sfu rpc: set deadline: %w", err)
		}
	}

	payload, err := proto.Marshal(req)
	if err != nil {
		return fmt.Errorf("sfu rpc: marshal request: %w", err)
	}
	if len(payload) > wireMaxPayload {
		return fmt.Errorf("sfu rpc: marshal request: %w", ErrPayloadTooLarge)
	}

	if err := writeFrame(conn, wireHeader{
		method: method,
		kind:   wireKindRequest,
		status: 0,
		length: uint32(len(payload)),
	}, payload); err != nil {
		return fmt.Errorf("sfu rpc: write request: %w", err)
	}

	header, body, err := readFrame(conn)
	if err != nil {
		return fmt.Errorf("sfu rpc: read response: %w", err)
	}
	if header.kind != wireKindResponse {
		return fmt.Errorf("sfu rpc: %w: got kind %d", ErrBadKind, header.kind)
	}
	if header.method != method {
		return fmt.Errorf("sfu rpc: %w: want %d got %d", ErrUnexpectedMethod, method, header.method)
	}
	if header.status != 0 {
		return fmt.Errorf("sfu rpc: %w %d", ErrRemoteStatus, header.status)
	}

	if err := proto.Unmarshal(body, rsp); err != nil {
		return fmt.Errorf("sfu rpc: unmarshal response: %w", err)
	}
	return nil
}

func requestDeadline(ctx context.Context, timeout time.Duration) (time.Time, bool) {
	var deadline time.Time
	haveDeadline := false

	if ctx != nil {
		if ctxDeadline, ok := ctx.Deadline(); ok {
			deadline = ctxDeadline
			haveDeadline = true
		}
	}

	if timeout > 0 {
		timeoutDeadline := time.Now().Add(timeout)
		if !haveDeadline || timeoutDeadline.Before(deadline) {
			deadline = timeoutDeadline
			haveDeadline = true
		}
	}

	return deadline, haveDeadline
}

func writeFrame(w io.Writer, header wireHeader, payload []byte) error {
	frame := make([]byte, wireHeaderSize+len(payload))
	frame[0] = wireMagic0
	frame[1] = wireMagic1
	frame[2] = wireMagic2
	frame[3] = wireMagic3
	frame[4] = wireVersion
	frame[5] = header.kind
	binary.BigEndian.PutUint16(frame[6:8], uint16(header.method))
	binary.BigEndian.PutUint16(frame[8:10], header.status)
	binary.BigEndian.PutUint32(frame[10:14], header.length)
	copy(frame[14:], payload)

	_, err := w.Write(frame)
	return err
}

func readFrame(r io.Reader) (wireHeader, []byte, error) {
	headerBuf := make([]byte, wireHeaderSize)
	if _, err := io.ReadFull(r, headerBuf); err != nil {
		return wireHeader{}, nil, err
	}

	if headerBuf[0] != wireMagic0 || headerBuf[1] != wireMagic1 || headerBuf[2] != wireMagic2 || headerBuf[3] != wireMagic3 {
		return wireHeader{}, nil, ErrBadMagic
	}
	if headerBuf[4] != wireVersion {
		return wireHeader{}, nil, ErrBadVersion
	}

	payloadLen := binary.BigEndian.Uint32(headerBuf[10:14])
	if payloadLen > wireMaxPayload {
		return wireHeader{}, nil, ErrPayloadTooLarge
	}

	payload := make([]byte, payloadLen)
	if _, err := io.ReadFull(r, payload); err != nil {
		return wireHeader{}, nil, err
	}

	return wireHeader{
		method: rpcMethod(binary.BigEndian.Uint16(headerBuf[6:8])),
		kind:   headerBuf[5],
		status: binary.BigEndian.Uint16(headerBuf[8:10]),
		length: payloadLen,
	}, payload, nil
}
