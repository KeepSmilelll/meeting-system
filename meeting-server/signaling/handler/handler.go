package handler

import "meeting-server/signaling/server"

type Handler interface {
	Handle(session *server.Session, payload []byte) error
}
