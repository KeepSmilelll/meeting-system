#pragma once

#include <cstdint>
#include <string>

namespace sfu {

class Subscriber final {
public:
    Subscriber() = default;
    explicit Subscriber(std::string userId,
                        std::string endpoint = {},
                        uint32_t audioSsrc = 0,
                        uint32_t videoSsrc = 0);

    const std::string& UserId() const noexcept;
    void SetUserId(std::string userId);

    const std::string& Endpoint() const noexcept;
    void SetEndpoint(std::string endpoint);

    uint32_t AudioSsrc() const noexcept;
    void SetAudioSsrc(uint32_t ssrc) noexcept;

    uint32_t VideoSsrc() const noexcept;
    void SetVideoSsrc(uint32_t ssrc) noexcept;

    bool Empty() const noexcept;

private:
    std::string userId_;
    std::string endpoint_;
    uint32_t audioSsrc_{0};
    uint32_t videoSsrc_{0};
};

} // namespace sfu
