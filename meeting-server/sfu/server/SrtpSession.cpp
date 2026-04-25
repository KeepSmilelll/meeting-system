#include "server/SrtpSession.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#ifdef HAVE_LIBSRTP2
#include <srtp2/srtp.h>
#endif

namespace sfu {
namespace {

#ifdef HAVE_LIBSRTP2

class SrtpLibraryGuard final {
public:
    SrtpLibraryGuard() = default;
    ~SrtpLibraryGuard() { Release(); }

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
        Release();
        acquired_ = other.acquired_;
        other.acquired_ = false;
        return *this;
    }

    bool Acquire(std::string* error) {
        std::lock_guard<std::mutex> lock(Mutex());
        if (RefCount() == 0) {
            const srtp_err_status_t status = srtp_init();
            if (status != srtp_err_status_ok) {
                if (error != nullptr) {
                    *error = "srtp_init failed: " + std::to_string(static_cast<int>(status));
                }
                return false;
            }
        }
        ++RefCount();
        acquired_ = true;
        return true;
    }

private:
    static std::mutex& Mutex() {
        static std::mutex value;
        return value;
    }

    static int& RefCount() {
        static int value = 0;
        return value;
    }

    void Release() {
        std::lock_guard<std::mutex> lock(Mutex());
        if (!acquired_ || RefCount() == 0) {
            return;
        }
        --RefCount();
        if (RefCount() == 0) {
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

using NativeSrtpSessionPtr = std::unique_ptr<srtp_ctx_t_, SrtpSessionDeleter>;

srtp_profile_t ToNativeProfile(SrtpSession::Profile profile) {
    switch (profile) {
    case SrtpSession::Profile::Aes128CmSha1_80:
    default:
        return srtp_profile_aes128_cm_sha1_80;
    }
}

std::string StatusToString(srtp_err_status_t status, const char* operation) {
    return std::string(operation) + " failed: " + std::to_string(static_cast<int>(status));
}

bool SetPolicyFromProfile(srtp_policy_t* policy,
                          srtp_profile_t profile,
                          std::string* error) {
    if (policy == nullptr) {
        if (error != nullptr) {
            *error = "SRTP policy is null";
        }
        return false;
    }

    const srtp_err_status_t rtpStatus =
        srtp_crypto_policy_set_from_profile_for_rtp(&policy->rtp, profile);
    if (rtpStatus != srtp_err_status_ok) {
        if (error != nullptr) {
            *error = StatusToString(rtpStatus, "srtp_crypto_policy_set_from_profile_for_rtp");
        }
        return false;
    }

    const srtp_err_status_t rtcpStatus =
        srtp_crypto_policy_set_from_profile_for_rtcp(&policy->rtcp, profile);
    if (rtcpStatus != srtp_err_status_ok) {
        if (error != nullptr) {
            *error = StatusToString(rtcpStatus, "srtp_crypto_policy_set_from_profile_for_rtcp");
        }
        return false;
    }

    return true;
}

int MaxRtpTrailerBytes() {
    return SRTP_MAX_TRAILER_LEN + 4;
}

int MaxRtcpTrailerBytes() {
#ifdef SRTP_MAX_SRTCP_TRAILER_LEN
    return SRTP_MAX_SRTCP_TRAILER_LEN + 4;
#else
    return SRTP_MAX_TRAILER_LEN + 4;
#endif
}

#endif

} // namespace

struct SrtpSession::Impl {
    std::vector<uint8_t> masterKey;
    std::vector<uint8_t> masterSalt;
    std::string lastError;

#ifdef HAVE_LIBSRTP2
    SrtpLibraryGuard library;
    NativeSrtpSessionPtr session;
#endif
};

SrtpSession::SrtpSession()
    : impl_(std::make_unique<Impl>()) {}

SrtpSession::~SrtpSession() = default;

SrtpSession::SrtpSession(SrtpSession&&) noexcept = default;

SrtpSession& SrtpSession::operator=(SrtpSession&&) noexcept = default;

bool SrtpSession::Configure(const std::vector<uint8_t>& masterKey,
                            const std::vector<uint8_t>& masterSalt,
                            Direction direction,
                            uint32_t ssrc,
                            Profile profile) {
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }

    Reset();
    impl_->lastError.clear();

    if (masterKey.empty() || masterSalt.empty()) {
        impl_->lastError = "SRTP keying material is empty";
        return false;
    }

#ifdef HAVE_LIBSRTP2
    const srtp_profile_t nativeProfile = ToNativeProfile(profile);
    const std::size_t expectedKeyBytes =
        srtp_profile_get_master_key_length(nativeProfile);
    const std::size_t expectedSaltBytes =
        srtp_profile_get_master_salt_length(nativeProfile);
    if (masterKey.size() != expectedKeyBytes || masterSalt.size() != expectedSaltBytes) {
        impl_->lastError = "SRTP key lengths mismatch";
        return false;
    }

    if (!impl_->library.Acquire(&impl_->lastError)) {
        return false;
    }

    std::vector<uint8_t> keyMaterial = masterKey;
    keyMaterial.insert(keyMaterial.end(), masterSalt.begin(), masterSalt.end());

    srtp_policy_t policy{};
    if (!SetPolicyFromProfile(&policy, nativeProfile, &impl_->lastError)) {
        return false;
    }
    policy.ssrc.type = ssrc == 0
        ? (direction == Direction::Inbound ? ssrc_any_inbound : ssrc_any_outbound)
        : ssrc_specific;
    policy.ssrc.value = ssrc;
    policy.key = keyMaterial.data();
    policy.next = nullptr;
    policy.window_size = 1024;
    policy.allow_repeat_tx = 1;

    srtp_t rawSession = nullptr;
    const srtp_err_status_t createStatus = srtp_create(&rawSession, &policy);
    if (createStatus != srtp_err_status_ok || rawSession == nullptr) {
        impl_->lastError = StatusToString(createStatus, "srtp_create");
        return false;
    }

    impl_->masterKey = masterKey;
    impl_->masterSalt = masterSalt;
    impl_->session.reset(rawSession);
    return true;
#else
    (void)direction;
    (void)ssrc;
    (void)profile;
    impl_->masterKey = masterKey;
    impl_->masterSalt = masterSalt;
    return true;
#endif
}

void SrtpSession::Reset() {
    if (!impl_) {
        return;
    }

#ifdef HAVE_LIBSRTP2
    impl_->session.reset();
#endif
    std::fill(impl_->masterKey.begin(), impl_->masterKey.end(), 0U);
    std::fill(impl_->masterSalt.begin(), impl_->masterSalt.end(), 0U);
    impl_->masterKey.clear();
    impl_->masterSalt.clear();
    impl_->lastError.clear();
}

bool SrtpSession::IsConfigured() const noexcept {
    return impl_ != nullptr && !impl_->masterKey.empty() && !impl_->masterSalt.empty();
}

const std::string& SrtpSession::LastError() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->lastError : kEmpty;
}

bool SrtpSession::ProtectRtp(std::vector<uint8_t>* packet) {
    if (packet == nullptr || packet->empty()) {
        if (impl_) {
            impl_->lastError = "SRTP RTP packet is empty";
        }
        return false;
    }
    if (!IsConfigured()) {
        if (impl_) {
            impl_->lastError = "SRTP session is not configured";
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!impl_->session) {
        impl_->lastError = "SRTP native session is not ready";
        return false;
    }

    const int originalSize = static_cast<int>(packet->size());
    packet->resize(packet->size() + static_cast<std::size_t>(MaxRtpTrailerBytes()));
    int protectedSize = originalSize;
    const srtp_err_status_t status =
        srtp_protect(impl_->session.get(), packet->data(), &protectedSize);
    if (status != srtp_err_status_ok || protectedSize <= originalSize) {
        packet->resize(static_cast<std::size_t>(originalSize));
        impl_->lastError = StatusToString(status, "srtp_protect");
        return false;
    }

    packet->resize(static_cast<std::size_t>(protectedSize));
    impl_->lastError.clear();
    return true;
#else
    impl_->lastError = "libsrtp2 is not available";
    return false;
#endif
}

bool SrtpSession::UnprotectRtp(std::vector<uint8_t>* packet) {
    if (packet == nullptr || packet->empty()) {
        if (impl_) {
            impl_->lastError = "SRTP RTP packet is empty";
        }
        return false;
    }
    if (!IsConfigured()) {
        if (impl_) {
            impl_->lastError = "SRTP session is not configured";
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!impl_->session) {
        impl_->lastError = "SRTP native session is not ready";
        return false;
    }

    int plainSize = static_cast<int>(packet->size());
    const srtp_err_status_t status =
        srtp_unprotect(impl_->session.get(), packet->data(), &plainSize);
    if (status != srtp_err_status_ok || plainSize <= 0) {
        impl_->lastError = StatusToString(status, "srtp_unprotect");
        return false;
    }

    packet->resize(static_cast<std::size_t>(plainSize));
    impl_->lastError.clear();
    return true;
#else
    impl_->lastError = "libsrtp2 is not available";
    return false;
#endif
}

bool SrtpSession::ProtectRtcp(std::vector<uint8_t>* packet) {
    if (packet == nullptr || packet->empty()) {
        if (impl_) {
            impl_->lastError = "SRTCP packet is empty";
        }
        return false;
    }
    if (!IsConfigured()) {
        if (impl_) {
            impl_->lastError = "SRTP session is not configured";
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!impl_->session) {
        impl_->lastError = "SRTP native session is not ready";
        return false;
    }

    const int originalSize = static_cast<int>(packet->size());
    packet->resize(packet->size() + static_cast<std::size_t>(MaxRtcpTrailerBytes()));
    int protectedSize = originalSize;
    const srtp_err_status_t status =
        srtp_protect_rtcp(impl_->session.get(), packet->data(), &protectedSize);
    if (status != srtp_err_status_ok || protectedSize <= originalSize) {
        packet->resize(static_cast<std::size_t>(originalSize));
        impl_->lastError = StatusToString(status, "srtp_protect_rtcp");
        return false;
    }

    packet->resize(static_cast<std::size_t>(protectedSize));
    impl_->lastError.clear();
    return true;
#else
    impl_->lastError = "libsrtp2 is not available";
    return false;
#endif
}

bool SrtpSession::UnprotectRtcp(std::vector<uint8_t>* packet) {
    if (packet == nullptr || packet->empty()) {
        if (impl_) {
            impl_->lastError = "SRTCP packet is empty";
        }
        return false;
    }
    if (!IsConfigured()) {
        if (impl_) {
            impl_->lastError = "SRTP session is not configured";
        }
        return false;
    }

#ifdef HAVE_LIBSRTP2
    if (!impl_->session) {
        impl_->lastError = "SRTP native session is not ready";
        return false;
    }

    int plainSize = static_cast<int>(packet->size());
    const srtp_err_status_t status =
        srtp_unprotect_rtcp(impl_->session.get(), packet->data(), &plainSize);
    if (status != srtp_err_status_ok || plainSize <= 0) {
        impl_->lastError = StatusToString(status, "srtp_unprotect_rtcp");
        return false;
    }

    packet->resize(static_cast<std::size_t>(plainSize));
    impl_->lastError.clear();
    return true;
#else
    impl_->lastError = "libsrtp2 is not available";
    return false;
#endif
}

std::size_t SrtpSession::MasterKeyLength(Profile profile) {
#ifdef HAVE_LIBSRTP2
    return srtp_profile_get_master_key_length(ToNativeProfile(profile));
#else
    (void)profile;
    return 16U;
#endif
}

std::size_t SrtpSession::MasterSaltLength(Profile profile) {
#ifdef HAVE_LIBSRTP2
    return srtp_profile_get_master_salt_length(ToNativeProfile(profile));
#else
    (void)profile;
    return 14U;
#endif
}

} // namespace sfu
