#include "server/RpcProtocol.h"

namespace sfu {
namespace {

void AppendU16(std::vector<uint8_t>* out, uint16_t value) {
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out->push_back(static_cast<uint8_t>(value & 0xFFU));
}

void AppendU32(std::vector<uint8_t>* out, uint32_t value) {
    out->push_back(static_cast<uint8_t>((value >> 24) & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 16) & 0xFFU));
    out->push_back(static_cast<uint8_t>((value >> 8) & 0xFFU));
    out->push_back(static_cast<uint8_t>(value & 0xFFU));
}

uint16_t ReadU16(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8U) |
                                 static_cast<uint16_t>(data[1]));
}

uint32_t ReadU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24U) |
           (static_cast<uint32_t>(data[1]) << 16U) |
           (static_cast<uint32_t>(data[2]) << 8U) |
           static_cast<uint32_t>(data[3]);
}

bool IsValidKind(uint8_t kind) {
    return kind == static_cast<uint8_t>(RpcFrameKind::kRequest) ||
           kind == static_cast<uint8_t>(RpcFrameKind::kResponse);
}

} // namespace

bool EncodeRpcFrame(const RpcFrame& frame, std::vector<uint8_t>* out) {
    if (out == nullptr || frame.payload.size() > kRpcMaxPayloadSize || !IsValidKind(static_cast<uint8_t>(frame.kind))) {
        return false;
    }

    out->clear();
    out->reserve(kRpcHeaderSize + frame.payload.size());

    AppendU32(out, kRpcMagic);
    out->push_back(kRpcVersion);
    out->push_back(static_cast<uint8_t>(frame.kind));
    AppendU16(out, static_cast<uint16_t>(frame.method));
    AppendU16(out, frame.status);
    AppendU32(out, static_cast<uint32_t>(frame.payload.size()));
    out->insert(out->end(), frame.payload.begin(), frame.payload.end());
    return true;
}

bool DecodeRpcFrame(const uint8_t* data, std::size_t len, RpcFrame* out) {
    if (out == nullptr || data == nullptr || len < kRpcHeaderSize) {
        return false;
    }

    const uint32_t magic = ReadU32(data);
    const uint8_t version = data[4];
    const uint8_t kind = data[5];
    const uint16_t method = ReadU16(data + 6);
    const uint16_t status = ReadU16(data + 8);
    const uint32_t payloadSize = ReadU32(data + 10);

    if (magic != kRpcMagic || version != kRpcVersion || !IsValidKind(kind) || payloadSize > kRpcMaxPayloadSize) {
        return false;
    }
    if (len != kRpcHeaderSize + payloadSize) {
        return false;
    }

    out->method = static_cast<RpcMethod>(method);
    out->kind = static_cast<RpcFrameKind>(kind);
    out->status = status;
    out->payload.assign(data + kRpcHeaderSize, data + len);
    return true;
}

} // namespace sfu
