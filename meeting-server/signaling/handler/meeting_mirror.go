package handler

import (
	"context"
	"errors"

	"meeting-server/signaling/protocol"
	"meeting-server/signaling/store"
)

type MeetingMirror interface {
	MirrorCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error
	MirrorJoin(ctx context.Context, meeting *store.Meeting, participant *protocol.Participant) error
	MirrorLeave(ctx context.Context, meetingID, userID, reason string) error
	MirrorTransferHost(ctx context.Context, meetingID, newHostUserID string) error
	MirrorDelete(ctx context.Context, meetingID string) error
}

func ComposeMeetingMirrors(mirrors ...MeetingMirror) MeetingMirror {
	filtered := make([]MeetingMirror, 0, len(mirrors))
	for _, mirror := range mirrors {
		if mirror != nil {
			filtered = append(filtered, mirror)
		}
	}
	switch len(filtered) {
	case 0:
		return nil
	case 1:
		return filtered[0]
	default:
		return compositeMeetingMirror(filtered)
	}
}

type compositeMeetingMirror []MeetingMirror

func (m compositeMeetingMirror) MirrorCreate(ctx context.Context, meeting *store.Meeting, host *protocol.Participant) error {
	return joinMirrorErrors(m.call(func(mirror MeetingMirror) error {
		return mirror.MirrorCreate(ctx, meeting, host)
	}))
}

func (m compositeMeetingMirror) MirrorJoin(ctx context.Context, meeting *store.Meeting, participant *protocol.Participant) error {
	return joinMirrorErrors(m.call(func(mirror MeetingMirror) error {
		return mirror.MirrorJoin(ctx, meeting, participant)
	}))
}

func (m compositeMeetingMirror) MirrorLeave(ctx context.Context, meetingID, userID, reason string) error {
	return joinMirrorErrors(m.call(func(mirror MeetingMirror) error {
		return mirror.MirrorLeave(ctx, meetingID, userID, reason)
	}))
}

func (m compositeMeetingMirror) MirrorTransferHost(ctx context.Context, meetingID, newHostUserID string) error {
	return joinMirrorErrors(m.call(func(mirror MeetingMirror) error {
		return mirror.MirrorTransferHost(ctx, meetingID, newHostUserID)
	}))
}

func (m compositeMeetingMirror) MirrorDelete(ctx context.Context, meetingID string) error {
	return joinMirrorErrors(m.call(func(mirror MeetingMirror) error {
		return mirror.MirrorDelete(ctx, meetingID)
	}))
}

func (m compositeMeetingMirror) call(fn func(MeetingMirror) error) []error {
	errs := make([]error, 0, len(m))
	for _, mirror := range m {
		if err := fn(mirror); err != nil {
			errs = append(errs, err)
		}
	}
	return errs
}

func joinMirrorErrors(errs []error) error {
	if len(errs) == 0 {
		return nil
	}
	return errors.Join(errs...)
}
