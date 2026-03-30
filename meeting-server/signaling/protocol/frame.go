package protocol

import (
    "encoding/binary"
    "errors"
)

const (
    HeaderSize     = 9
    MagicHigh byte = 0xAB
    MagicLow  byte = 0xCD
    Version   byte = 0x01
)

var (
    ErrFrameTooShort      = errors.New("frame too short")
    ErrBadMagic           = errors.New("bad frame magic")
    ErrUnsupportedVersion = errors.New("unsupported frame version")
    ErrPayloadLength      = errors.New("invalid frame payload length")
)

func EncodeFrame(msgType SignalType, payload []byte) []byte {
    frame := make([]byte, HeaderSize+len(payload))
    frame[0] = MagicHigh
    frame[1] = MagicLow
    frame[2] = Version
    binary.BigEndian.PutUint16(frame[3:5], uint16(msgType))
    binary.BigEndian.PutUint32(frame[5:9], uint32(len(payload)))
    copy(frame[9:], payload)
    return frame
}

func DecodeHeader(header []byte, maxPayload uint32) (SignalType, uint32, error) {
    if len(header) != HeaderSize {
        return 0, 0, ErrFrameTooShort
    }
    if header[0] != MagicHigh || header[1] != MagicLow {
        return 0, 0, ErrBadMagic
    }
    if header[2] != Version {
        return 0, 0, ErrUnsupportedVersion
    }

    msgType := SignalType(binary.BigEndian.Uint16(header[3:5]))
    payloadLen := binary.BigEndian.Uint32(header[5:9])
    if payloadLen > maxPayload {
        return 0, 0, ErrPayloadLength
    }
    return msgType, payloadLen, nil
}

