#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace av::codec {

struct DecodedChunk {
    std::vector<uint8_t> samples;
    int64_t pts{0};
};

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual bool reset() = 0;
    virtual bool decode(const uint8_t* data,
                        std::size_t size,
                        int64_t pts,
                        DecodedChunk& outChunk,
                        std::string* error = nullptr) = 0;
};

}  // namespace av::codec
