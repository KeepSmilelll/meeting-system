package handler

import (
	"context"
	"errors"
	"fmt"

	"meeting-server/signaling/protocol"
	pb "meeting-server/signaling/protocol/pb"
	signalingSfu "meeting-server/signaling/sfu"
	"meeting-server/signaling/store"
)

type SFUMeetingMirror struct {
	client signalingSfu.Client
}

func NewSFUMeetingMirror(client signalingSfu.Client) MeetingMirror {
	if client == nil {
		return nil
	}
	return &SFUMeetingMirror{client: client}
}

func (m *SFUMeetingMirror) MirrorCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error {
	_ = host
	if m == nil || m.client == nil || meeting == nil {
		return nil
	}

	rsp, err := m.client.CreateRoom(ctx, &pb.CreateRoomReq{
		MeetingId:     meeting.ID,
		MaxPublishers: int32(meeting.MaxParticipants),
	})
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("sfu meeting mirror: create room %s: %w", meeting.ID, err)
	}
	if rsp == nil || !rsp.GetSuccess() {
		return fmt.Errorf("sfu meeting mirror: create room %s: unsuccessful response", meeting.ID)
	}
	return nil
}

func (m *SFUMeetingMirror) MirrorJoin(context.Context, *store.Meeting, *protocol.Participant) error {
	return nil
}

func (m *SFUMeetingMirror) MirrorLeave(ctx context.Context, meetingID, userID, reason string) error {
	_ = reason
	if m == nil || m.client == nil || meetingID == "" || userID == "" {
		return nil
	}

	rsp, err := m.client.RemovePublisher(ctx, &pb.RemovePublisherReq{
		MeetingId: meetingID,
		UserId:    userID,
	})
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("sfu meeting mirror: remove publisher meeting=%s user=%s: %w", meetingID, userID, err)
	}
	if rsp == nil || !rsp.GetSuccess() {
		return nil
	}
	return nil
}

func (m *SFUMeetingMirror) MirrorTransferHost(context.Context, string, string) error {
	return nil
}

func (m *SFUMeetingMirror) MirrorDelete(ctx context.Context, meetingID string) error {
	if m == nil || m.client == nil || meetingID == "" {
		return nil
	}

	rsp, err := m.client.DestroyRoom(ctx, &pb.DestroyRoomReq{MeetingId: meetingID})
	if errors.Is(err, signalingSfu.ErrDisabled) {
		return nil
	}
	if err != nil {
		return fmt.Errorf("sfu meeting mirror: destroy room %s: %w", meetingID, err)
	}
	if rsp == nil || !rsp.GetSuccess() {
		return fmt.Errorf("sfu meeting mirror: destroy room %s: unsuccessful response", meetingID)
	}
	return nil
}
