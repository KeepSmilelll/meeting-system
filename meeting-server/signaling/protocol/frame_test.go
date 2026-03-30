package protocol

import "testing"

func TestEncodeAndDecodeHeader(t *testing.T) {
	payload := []byte("hello")
	frame := EncodeFrame(AuthHeartbeatReq, payload)
	if len(frame) != HeaderSize+len(payload) {
		t.Fatalf("unexpected frame length: %d", len(frame))
	}

	msgType, payloadLen, err := DecodeHeader(frame[:HeaderSize], 1024)
	if err != nil {
		t.Fatalf("DecodeHeader failed: %v", err)
	}
	if msgType != AuthHeartbeatReq {
		t.Fatalf("unexpected msg type: %v", msgType)
	}
	if payloadLen != uint32(len(payload)) {
		t.Fatalf("unexpected payload len: %d", payloadLen)
	}
}

func TestDecodeHeaderBadMagic(t *testing.T) {
	header := make([]byte, HeaderSize)
	header[0] = 0x00
	header[1] = 0x01
	header[2] = Version

	if _, _, err := DecodeHeader(header, 1024); err != ErrBadMagic {
		t.Fatalf("expected ErrBadMagic, got: %v", err)
	}
}

func TestDecodeHeaderTooLarge(t *testing.T) {
	frame := EncodeFrame(AuthHeartbeatReq, make([]byte, 10))
	if _, _, err := DecodeHeader(frame[:HeaderSize], 5); err != ErrPayloadLength {
		t.Fatalf("expected ErrPayloadLength, got: %v", err)
	}
}
