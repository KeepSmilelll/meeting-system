package handler

import (
	"context"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/server"
	"meeting-server/signaling/store"

	"google.golang.org/protobuf/proto"
)

func syncSessionPresence(ctx context.Context, cfg config.Config, sessionStore store.SessionStore, session *server.Session) {
	if sessionStore == nil || session == nil || session.UserID() == "" {
		return
	}

	ttl := cfg.TokenTTL
	if ttl <= 0 {
		ttl = 24 * time.Hour
	}

	_ = sessionStore.Upsert(ctx, store.SessionPresence{
		UserID:    session.UserID(),
		NodeID:    cfg.NodeID,
		SessionID: session.ID,
		MeetingID: session.MeetingID(),
		Status:    int32(session.State()),
	}, ttl)
}

func clearSessionPresence(ctx context.Context, sessionStore store.SessionStore, session *server.Session) {
	if sessionStore == nil || session == nil || session.UserID() == "" {
		return
	}

	_ = sessionStore.Delete(ctx, session.UserID(), session.ID)
}

func mustMarshalProto(message proto.Message) []byte {
	if message == nil {
		return nil
	}

	payload, err := proto.Marshal(message)
	if err != nil {
		return nil
	}
	return payload
}
