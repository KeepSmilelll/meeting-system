#include "SRTPContext.h"

#include "CryptoUtils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <utility>

#include <QtGlobal>

#ifdef HAVE_LIBSRTP2
#include <srtp2/srtp.h>
#endif

namespace security {
namespace {

#ifdef HAVE_LIBSRTP2

class SrtpLibraryGuard final {
public:
    SrtpLibraryGuard() = default;
    ~SrtpLibraryGuard() { release(); }

    SrtpLibraryGuard(const SrtpLibraryGuard&) = delete;
    SrtpLibraryGuard& operator=(const SrtpLibraryGuard&) = delete;

    SrtpLibraryGuard(SrtpLibraryGuard&& other) noexcept
        : acquired_(other.acquired_) {
        other.acquired_ = false;
    }

    SrtpLibraryGuard& operator=(SrtpLibraryGuard&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        release();
        acquired_ = other.acquired_;
        other.acquired_ = false;
        return *this;
    }

    bool acquire(QString* error) {
        std::lock_guard<std::mutex> lock(mutex());
        if (refCount() == 0) {
            const srtp_err_status_t status = srtp_init();
            if (status != srtp_err_status_ok) {
                if (error != nullptr) {
                    *error = QStringLiteral("srtp_init failed: %1").arg(static_cast<int>(status));
                }
                return false;
            }
        }
        ++refCount();
        acquired_ = true;
        return true;
    }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    static int& refCount() {
        static int value = 0;
        return value;
    }

    void release() {
        std::lock_guard<std::mutex> lock(mutex());
        if (!acquired_ || refCount() == 0) {
            return;
        }
        --refCount();
        if (refCount() == 0) {
            (void)srtp_shutdown();
        }
        acquired_ = false;
    }

    bool acquired_{false};
};

struct SrtpSessionDeleter {
    void operator()(srtp_ctx_t_* session) const noexcept {
        if (session != nullptr) {
            srtp_dealloc(session);
        }
    }
};

using SrtpSessionPtr = std::unique_ptr<srtp_ctx_t_, SrtpSessionDeleter>;

srtp_profile_t toNativeProfile(SRTPContext::Profile profile) {
    switch (profile) {
    case SRTPContext::Profile::Aes128CmSha1_80:
    default:
        return srtp_profile_aes128_cm_sha1_80;
    }
}

QString statusToString(srtp_err_status_t status, const char* operation) {
    return QStringLiteral("%1 failed: %2")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<int>(status));
}

bool setPolicyFromProfile(srtp_policy_t* policy,
                          srtp_profile_t profile,
                          QString* error) {
    if (policy == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("SRTP policy is null");
        }
        return false;
    }

    const srtp_err_status_t rtpStatus =
        srtp_crypto_policy_set_from_profile_for_rtp(&policy->rtp, profile);
    if (rtpStatus != srtp_err_status_ok) {
        if (error != nullptr) {
            *error = statusToString(rtpStatus, "srtp_crypto_policy_set_from_profile_for_rtp");
        }
        return false;
    }

    const srtp_err_status_t rtcpStatus =
        srtp_crypto_policy_set_from_profile_for_rtcp(&policy->rtcp, profile);
    if (rtcpStatus != srtp_err_status_ok) {
        if (error != nullptr) {
            *error = statusToString(rtcpStatus, "srtp_crypto_policy_set_from_profile_for_rtcp");
        }
        return false;
    }

    return true;
}

int maxRtpTrailerBytes() {
    return SRTP_MAX_TRAILER_LEN + 4;
}

int maxRtcpTrailerBytes() {
    return SRTP_MAX_SRTCP_TRAILER_LEN + 4;
}

#endif

}  // namespace

struct SRTPContext::Impl {
    QByteArray masterKey;
    QByteArray masterSalt;
    QString lastError;

#ifdef HAVE_LIBSRTP2
    SrtpLibraryGuard library;
    SrtpSessionPtr session;
    Direction direction{Direction::Outbound};
    Profile profile{Profile::Aes128CmSha1_80};
    uint32_t ssrc{0};
#endif
};

SRTPContext::SRTPContext()
    : m_impl(std::make_unique<Impl>()) {}

SRTPContext::~SRTPContext() = default;

SRTPContext::SRTPContext(SRTPContext&&) noexcept = default;

SRTPContext& SRTPContext::operator=(SRTPContext&&) noexcept = default;

bool SRTPContext::configure(const QByteArray& masterKey,
                            const QByteArray& masterSalt,
                            Direction direction,
                            uint32_t ssrc,
                            Profile profile) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }

    clear();
    m_impl->lastError.clear();

    if (masterKey.isEmpty() || masterSalt.isEmpty()) {
        m_impl->lastError = QStringLiteral("SRTP keying material is empty");
        return false;
    }

#ifdef HAVE_LIBSRTP2
    const srtp_profile_t nativeProfile = toNativeProfile(profile);
    const int expectedKeyBytes =
        static_cast<int>(srtp_profile_get_master_key_length(nativeProfile));
    const int expectedSaltBytes =
        static_cast<int>(srtp_profile_get_master_salt_length(nativeProfile));
    if (masterKey.size() != expectedKeyBytes || masterSalt.size() != expectedSaltBytes) {
        m_impl->lastError =
            QStringLiteral("SRTP key lengths mismatch: expected %1+%2 got %3+%4")
                .arg(expectedKeyBytes)
                .arg(expectedSaltBytes)
                .arg(masterKey.size())
                .arg(masterSalt.size());
        return false;
    }

    if (!m_impl->library.acquire(&m_impl->lastError)) {
        return false;
    }

    QByteArray masterKeyMaterial = masterKey;
    masterKeyMaterial.append(masterSalt);
    srtp_policy_t policy{};
    if (!setPolicyFromProfile(&policy, nativeProfile, &m_impl->lastError)) {
        return false;
    }
    policy.ssrc.type = ssrc == 0
        ? (direction == Direction::Inbound ? ssrc_any_inbound : ssrc_any_outbound)
        : ssrc_specific;
    policy.ssrc.value = ssrc;
    policy.key = reinterpret_cast<unsigned char*>(masterKeyMaterial.data());
    policy.next = nullptr;
    policy.window_size = 1024;
    policy.allow_repeat_tx = 1;

    srtp_t rawSession = nullptr;
    const srtp_err_status_t createStatus = srtp_create(&rawSession, &policy);
    if (createStatus != srtp_err_status_ok || rawSession == nullptr) {
        m_impl->lastError = statusToString(createStatus, "srtp_create");
        return false;
    }

    m_impl->masterKey = masterKey;
    m_impl->masterSalt = masterSalt;
    m_impl->session.reset(rawSession);
    m_impl->direction = direction;
    m_impl->profile = profile;
    m_impl->ssrc = ssrc;
    return true;
#else
    Q_UNUSED(direction);
    Q_UNUSED(ssrc);
    Q_UNUSED(profile);
    m_impl->masterKey = masterKey;
    m_impl->masterSalt = masterSalt;
    return true;
#endif
}

void SRTPContext::clear() {
    if (!m_impl) {
        return;
    }

    m_impl->masterKey.fill('\0');
    m_impl->masterSalt.fill('\0');
    m_impl->masterKey.clear();
    m_impl->masterSalt.clear();
    m_impl->lastError.clear();
#ifdef HAVE_LIBSRTP2
    m_impl->session.reset();
    m_impl->ssrc = 0;
#endif
}

bool SRTPContext::configured() const {
    return m_impl && !m_impl->masterKey.isEmpty() && !m_impl->masterSalt.isEmpty();
}

QString SRTPContext::keyFingerprint() const {
    if (!configured()) {
        return {};
    }
    return CryptoUtils::sha256Hex(m_impl->masterKey + m_impl->masterSalt).left(16);
}

QString SRTPContext::lastError() const {
    return m_impl ? m_impl->lastError : QString{};
}

bool SRTPContext::protectRtp(QByteArray* packet) {
    if (packet == nullptr || packet->isEmpty()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP RTP packet is empty");
        }
        return false;
    }
    if (!configured()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP context not configured");
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!m_impl->session) {
        m_impl->lastError = QStringLiteral("SRTP session is not ready");
        return false;
    }

    const int originalSize = packet->size();
    packet->resize(originalSize + maxRtpTrailerBytes());
    int protectedSize = originalSize;
    const srtp_err_status_t status =
        srtp_protect(m_impl->session.get(), packet->data(), &protectedSize);
    if (status != srtp_err_status_ok || protectedSize <= originalSize) {
        packet->resize(originalSize);
        m_impl->lastError = statusToString(status, "srtp_protect");
        return false;
    }
    packet->resize(protectedSize);
    m_impl->lastError.clear();
    return true;
#else
    Q_UNUSED(packet);
    m_impl->lastError = QStringLiteral("libsrtp2 is not available");
    return false;
#endif
}

bool SRTPContext::unprotectRtp(QByteArray* packet) {
    if (packet == nullptr || packet->isEmpty()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP RTP packet is empty");
        }
        return false;
    }
    if (!configured()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP context not configured");
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!m_impl->session) {
        m_impl->lastError = QStringLiteral("SRTP session is not ready");
        return false;
    }

    int plainSize = packet->size();
    const srtp_err_status_t status =
        srtp_unprotect(m_impl->session.get(), packet->data(), &plainSize);
    if (status != srtp_err_status_ok || plainSize <= 0) {
        m_impl->lastError = statusToString(status, "srtp_unprotect");
        return false;
    }
    packet->resize(plainSize);
    m_impl->lastError.clear();
    return true;
#else
    Q_UNUSED(packet);
    m_impl->lastError = QStringLiteral("libsrtp2 is not available");
    return false;
#endif
}

bool SRTPContext::protectRtcp(QByteArray* packet) {
    if (packet == nullptr || packet->isEmpty()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTCP packet is empty");
        }
        return false;
    }
    if (!configured()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP context not configured");
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!m_impl->session) {
        m_impl->lastError = QStringLiteral("SRTP session is not ready");
        return false;
    }

    const int originalSize = packet->size();
    packet->resize(originalSize + maxRtcpTrailerBytes());
    int protectedSize = originalSize;
    const srtp_err_status_t status =
        srtp_protect_rtcp(m_impl->session.get(), packet->data(), &protectedSize);
    if (status != srtp_err_status_ok || protectedSize <= originalSize) {
        packet->resize(originalSize);
        m_impl->lastError = statusToString(status, "srtp_protect_rtcp");
        return false;
    }
    packet->resize(protectedSize);
    m_impl->lastError.clear();
    return true;
#else
    Q_UNUSED(packet);
    m_impl->lastError = QStringLiteral("libsrtp2 is not available");
    return false;
#endif
}

bool SRTPContext::unprotectRtcp(QByteArray* packet) {
    if (packet == nullptr || packet->isEmpty()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTCP packet is empty");
        }
        return false;
    }
    if (!configured()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("SRTP context not configured");
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!m_impl->session) {
        m_impl->lastError = QStringLiteral("SRTP session is not ready");
        return false;
    }

    int plainSize = packet->size();
    const srtp_err_status_t status =
        srtp_unprotect_rtcp(m_impl->session.get(), packet->data(), &plainSize);
    if (status != srtp_err_status_ok || plainSize <= 0) {
        m_impl->lastError = statusToString(status, "srtp_unprotect_rtcp");
        return false;
    }
    packet->resize(plainSize);
    m_impl->lastError.clear();
    return true;
#else
    Q_UNUSED(packet);
    m_impl->lastError = QStringLiteral("libsrtp2 is not available");
    return false;
#endif
}

int SRTPContext::masterKeyLength(Profile profile) {
#ifdef HAVE_LIBSRTP2
    return static_cast<int>(srtp_profile_get_master_key_length(toNativeProfile(profile)));
#else
    Q_UNUSED(profile);
    return 16;
#endif
}

int SRTPContext::masterSaltLength(Profile profile) {
#ifdef HAVE_LIBSRTP2
    return static_cast<int>(srtp_profile_get_master_salt_length(toNativeProfile(profile)));
#else
    Q_UNUSED(profile);
    return 14;
#endif
}

}  // namespace security
