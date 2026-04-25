#pragma once

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>

namespace security {

class SRTPContext {
public:
    enum class Profile {
        Aes128CmSha1_80,
    };

    enum class Direction {
        Inbound,
        Outbound,
    };

    SRTPContext();
    ~SRTPContext();

    SRTPContext(const SRTPContext&) = delete;
    SRTPContext& operator=(const SRTPContext&) = delete;
    SRTPContext(SRTPContext&&) noexcept;
    SRTPContext& operator=(SRTPContext&&) noexcept;

    bool configure(const QByteArray& masterKey,
                   const QByteArray& masterSalt,
                   Direction direction = Direction::Outbound,
                   uint32_t ssrc = 0,
                   Profile profile = Profile::Aes128CmSha1_80);
    void clear();

    bool configured() const;
    QString keyFingerprint() const;
    QString lastError() const;

    bool protectRtp(QByteArray* packet);
    bool unprotectRtp(QByteArray* packet);
    bool protectRtcp(QByteArray* packet);
    bool unprotectRtcp(QByteArray* packet);

    static int masterKeyLength(Profile profile = Profile::Aes128CmSha1_80);
    static int masterSaltLength(Profile profile = Profile::Aes128CmSha1_80);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

}  // namespace security
