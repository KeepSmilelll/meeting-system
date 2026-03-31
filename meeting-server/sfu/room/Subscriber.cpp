#include "Subscriber.h"

#include <utility>

namespace sfu {

Subscriber::Subscriber(std::string userId,
                       std::string endpoint,
                       uint32_t audioSsrc,
                       uint32_t videoSsrc)
    : userId_(std::move(userId))
    , endpoint_(std::move(endpoint))
    , audioSsrc_(audioSsrc)
    , videoSsrc_(videoSsrc) {}

const std::string& Subscriber::UserId() const noexcept {
    return userId_;
}

void Subscriber::SetUserId(std::string userId) {
    userId_ = std::move(userId);
}

const std::string& Subscriber::Endpoint() const noexcept {
    return endpoint_;
}

void Subscriber::SetEndpoint(std::string endpoint) {
    endpoint_ = std::move(endpoint);
}

uint32_t Subscriber::AudioSsrc() const noexcept {
    return audioSsrc_;
}

void Subscriber::SetAudioSsrc(uint32_t ssrc) noexcept {
    audioSsrc_ = ssrc;
}

uint32_t Subscriber::VideoSsrc() const noexcept {
    return videoSsrc_;
}

void Subscriber::SetVideoSsrc(uint32_t ssrc) noexcept {
    videoSsrc_ = ssrc;
}

bool Subscriber::Empty() const noexcept {
    return userId_.empty();
}

} // namespace sfu
