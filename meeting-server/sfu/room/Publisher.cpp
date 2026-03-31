#include "Publisher.h"

#include <utility>

namespace sfu {

Publisher::Publisher(std::string userId, uint32_t audioSsrc, uint32_t videoSsrc)
    : userId_(std::move(userId))
    , audioSsrc_(audioSsrc)
    , videoSsrc_(videoSsrc) {}

const std::string& Publisher::UserId() const noexcept {
    return userId_;
}

void Publisher::SetUserId(std::string userId) {
    userId_ = std::move(userId);
}

uint32_t Publisher::AudioSsrc() const noexcept {
    return audioSsrc_;
}

void Publisher::SetAudioSsrc(uint32_t ssrc) noexcept {
    audioSsrc_ = ssrc;
}

uint32_t Publisher::VideoSsrc() const noexcept {
    return videoSsrc_;
}

void Publisher::SetVideoSsrc(uint32_t ssrc) noexcept {
    videoSsrc_ = ssrc;
}

bool Publisher::Empty() const noexcept {
    return userId_.empty();
}

} // namespace sfu
