package handler

import (
	"fmt"
	"time"

	"meeting-server/signaling/config"
	"meeting-server/signaling/protocol"
	"meeting-server/signaling/server"
)

const maxFileTransferSizeBytes int64 = 512 * 1024 * 1024 // 512MB

type FileHandler struct {
	cfg      config.Config
	sessions *server.SessionManager
}

func NewFileHandler(cfg config.Config, sessions *server.SessionManager) *FileHandler {
	return &FileHandler{cfg: cfg, sessions: sessions}
}

func (h *FileHandler) HandleOffer(session *server.Session, payload []byte) {
	var req protocol.FileOfferReqBody
	if !decodeProto(payload, &req) || req.FileName == "" || req.FileSize <= 0 {
		_ = session.Send(protocol.FileOfferRsp, &protocol.FileOfferRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: protocol.ErrInvalidParam, Message: "bad file offer request"},
		})
		return
	}

	if req.FileSize > maxFileTransferSizeBytes {
		_ = session.Send(protocol.FileOfferRsp, &protocol.FileOfferRspBody{
			Success: false,
			Error:   &protocol.ErrorInfo{Code: protocol.ErrFileTooLarge, Message: "file is too large"},
		})
		return
	}

	// File transfer pipeline is not enabled yet. Keep protocol behavior explicit.
	_ = session.Send(protocol.FileOfferRsp, &protocol.FileOfferRspBody{
		Success:    false,
		Error:      &protocol.ErrorInfo{Code: protocol.ErrTransferRejected, Message: "file transfer not enabled"},
		TransferId: fmt.Sprintf("disabled-%d", time.Now().UnixMilli()),
	})
}

func (h *FileHandler) HandleAccept(session *server.Session, payload []byte) {
	var req protocol.FileAcceptReqBody
	if !decodeProto(payload, &req) || req.TransferId == "" {
		_ = session.Send(protocol.FileCompleteNotify, &protocol.FileCompleteNotifyBody{
			TransferId: req.TransferId,
			Success:    false,
			FileHash:   "",
		})
		return
	}

	_ = session.Send(protocol.FileCompleteNotify, &protocol.FileCompleteNotifyBody{
		TransferId: req.TransferId,
		Success:    false,
		FileHash:   "",
	})
}

func (h *FileHandler) HandleChunk(session *server.Session, payload []byte) {
	var req protocol.FileChunkDataBody
	if !decodeProto(payload, &req) || req.TransferId == "" {
		return
	}

	_ = session.Send(protocol.FileCompleteNotify, &protocol.FileCompleteNotifyBody{
		TransferId: req.TransferId,
		Success:    false,
		FileHash:   "",
	})
}
