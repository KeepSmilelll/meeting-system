#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sfu {

enum class RpcMethod : uint16_t {
    kCreateRoom = 1,
    kDestroyRoom = 2,
    kAddPublisher = 3,
    kRemovePublisher = 4,
    kGetNodeStatus = 5,
    kReportNodeStatus = 6,
    kQualityReport = 7,
};

enum class RpcFrameKind : uint8_t {
    kRequest = 1,
    kResponse = 2,
};

struct RpcFrame final {
    RpcMethod method{RpcMethod::kCreateRoom};
    RpcFrameKind kind{RpcFrameKind::kRequest};
    uint16_t status{0};
    std::vector<uint8_t> payload;
};

constexpr uint32_t kRpcMagic = 0x53465552U; // 'SFUR'
constexpr uint8_t kRpcVersion = 1;
constexpr std::size_t kRpcHeaderSize = 14;
constexpr std::size_t kRpcMaxPayloadSize = 1024U * 1024U;

bool EncodeRpcFrame(const RpcFrame& frame, std::vector<uint8_t>* out);
bool DecodeRpcFrame(const uint8_t* data, std::size_t len, RpcFrame* out);

} // namespace sfu
