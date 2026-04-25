#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sfu {

class SrtpSession final {
public:
    enum class Profile {
        Aes128CmSha1_80,
    };

    enum class Direction {
        Inbound,
        Outbound,
    };

    SrtpSession();
    ~SrtpSession();

    SrtpSession(const SrtpSession&) = delete;
    SrtpSession& operator=(const SrtpSession&) = delete;
    SrtpSession(SrtpSession&&) noexcept;
    SrtpSession& operator=(SrtpSession&&) noexcept;

    bool Configure(const std::vector<uint8_t>& masterKey,
                   const std::vector<uint8_t>& masterSalt,
                   Direction direction = Direction::Outbound,
                   uint32_t ssrc = 0,
                   Profile profile = Profile::Aes128CmSha1_80);
    void Reset();

    bool IsConfigured() const noexcept;
    const std::string& LastError() const noexcept;

    bool ProtectRtp(std::vector<uint8_t>* packet);
    bool UnprotectRtp(std::vector<uint8_t>* packet);
    bool ProtectRtcp(std::vector<uint8_t>* packet);
    bool UnprotectRtcp(std::vector<uint8_t>* packet);

    static std::size_t MasterKeyLength(Profile profile = Profile::Aes128CmSha1_80);
    static std::size_t MasterSaltLength(Profile profile = Profile::Aes128CmSha1_80);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sfu
