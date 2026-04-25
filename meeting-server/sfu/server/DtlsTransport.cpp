#include "server/DtlsTransport.h"

#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace sfu {
namespace {

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept {
        SSL_free(ssl);
    }
};

struct X509Deleter {
    void operator()(X509* cert) const noexcept {
        X509_free(cert);
    }
};

using SslPtr = std::unique_ptr<SSL, SslDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

std::string NormalizeFingerprint(std::string fingerprint) {
    std::string normalized;
    normalized.reserve(fingerprint.size());
    for (const char ch : fingerprint) {
        if (ch == ':' || std::isxdigit(static_cast<unsigned char>(ch)) != 0) {
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
        }
    }
    return normalized;
}

std::string FormatFingerprint(const unsigned char* digest, unsigned int digestLen) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        if (i != 0U) {
            stream << ':';
        }
        stream << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return stream.str();
}

std::string CurrentSslError(const char* prefix) {
    const unsigned long code = ERR_get_error();
    if (code == 0UL) {
        return std::string(prefix) + ": unknown OpenSSL error";
    }

    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    return std::string(prefix) + ": " + buffer.data();
}

bool DrainBio(BIO* bio, std::vector<std::vector<uint8_t>>* outgoingPackets, std::string* error) {
    if (outgoingPackets == nullptr || bio == nullptr) {
        return true;
    }

    while (BIO_ctrl_pending(bio) > 0) {
        const long pending = BIO_ctrl_pending(bio);
        if (pending <= 0) {
            break;
        }
        std::vector<uint8_t> packet(static_cast<std::size_t>(pending));
        const int read = BIO_read(bio, packet.data(), static_cast<int>(packet.size()));
        if (read <= 0) {
            if (error != nullptr) {
                *error = "DTLS write BIO drain failed";
            }
            return false;
        }
        packet.resize(static_cast<std::size_t>(read));
        outgoingPackets->push_back(std::move(packet));
    }
    return true;
}

bool ExportPeerFingerprint(SSL* ssl, std::string* outFingerprint) {
    if (ssl == nullptr || outFingerprint == nullptr) {
        return false;
    }

    X509Ptr cert(SSL_get1_peer_certificate(ssl));
    if (!cert) {
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digestLen = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &digestLen) != 1 || digestLen == 0U) {
        return false;
    }
    *outFingerprint = FormatFingerprint(digest, digestLen);
    return true;
}

} // namespace

struct DtlsTransport::Impl {
    explicit Impl(const DtlsContext& dtlsContext, Role dtlsRole)
        : context(dtlsContext)
        , role(dtlsRole) {}

    const DtlsContext& context;
    Role role{Role::Server};
    SslPtr ssl;
    BIO* readBio{nullptr};
    BIO* writeBio{nullptr};
    bool started{false};
    bool connected{false};
    std::string expectedPeerFingerprint;
    std::string peerFingerprint;
    std::string selectedSrtpProfile;
    std::string lastError;
};

DtlsTransport::DtlsTransport(const DtlsContext& context, Role role)
    : impl_(std::make_unique<Impl>(context, role)) {}

DtlsTransport::~DtlsTransport() = default;

DtlsTransport::DtlsTransport(DtlsTransport&&) noexcept = default;

DtlsTransport& DtlsTransport::operator=(DtlsTransport&&) noexcept = default;

bool DtlsTransport::Start(const std::string& expectedPeerFingerprint,
                          std::vector<std::vector<uint8_t>>* outgoingPackets) {
    if (!impl_ || !impl_->context.IsReady() || impl_->context.RawContext() == nullptr) {
        if (impl_) {
            impl_->lastError = "DTLS context is not ready";
        }
        return false;
    }
    if (impl_->started) {
        return true;
    }

    SSL* rawSsl = SSL_new(impl_->context.RawContext());
    if (rawSsl == nullptr) {
        impl_->lastError = CurrentSslError("SSL_new");
        return false;
    }

    BIO* readBio = BIO_new(BIO_s_mem());
    BIO* writeBio = BIO_new(BIO_s_mem());
    if (readBio == nullptr || writeBio == nullptr) {
        if (readBio != nullptr) {
            BIO_free(readBio);
        }
        if (writeBio != nullptr) {
            BIO_free(writeBio);
        }
        SSL_free(rawSsl);
        impl_->lastError = "BIO_new failed";
        return false;
    }

    BIO_set_mem_eof_return(readBio, -1);
    BIO_set_mem_eof_return(writeBio, -1);
    SSL_set_bio(rawSsl, readBio, writeBio);
    SSL_set_mtu(rawSsl, 1200);
    if (impl_->role == Role::Server) {
        SSL_set_accept_state(rawSsl);
    } else {
        SSL_set_connect_state(rawSsl);
    }

    impl_->ssl.reset(rawSsl);
    impl_->readBio = readBio;
    impl_->writeBio = writeBio;
    impl_->expectedPeerFingerprint = NormalizeFingerprint(expectedPeerFingerprint);
    impl_->started = true;
    impl_->lastError.clear();

    const int ret = SSL_do_handshake(impl_->ssl.get());
    if (ret != 1) {
        const int error = SSL_get_error(impl_->ssl.get(), ret);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            impl_->lastError = CurrentSslError("SSL_do_handshake");
            return false;
        }
    }
    return DrainBio(impl_->writeBio, outgoingPackets, &impl_->lastError);
}

bool DtlsTransport::HandleIncomingDatagram(const uint8_t* data,
                                           std::size_t len,
                                           std::vector<std::vector<uint8_t>>* outgoingPackets) {
    if (!impl_ || !impl_->started || impl_->ssl == nullptr || impl_->readBio == nullptr || data == nullptr || len == 0U) {
        if (impl_) {
            impl_->lastError = "DTLS transport is not ready for incoming datagrams";
        }
        return false;
    }

    const int written = BIO_write(impl_->readBio, data, static_cast<int>(len));
    if (written != static_cast<int>(len)) {
        impl_->lastError = "BIO_write failed for DTLS datagram";
        return false;
    }

    const int ret = SSL_do_handshake(impl_->ssl.get());
    if (ret == 1) {
        impl_->connected = true;
        if (!ExportPeerFingerprint(impl_->ssl.get(), &impl_->peerFingerprint)) {
            impl_->lastError = "DTLS peer certificate fingerprint export failed";
            return false;
        }
        if (!impl_->expectedPeerFingerprint.empty() &&
            NormalizeFingerprint(impl_->peerFingerprint) != impl_->expectedPeerFingerprint) {
            impl_->lastError = "DTLS peer fingerprint mismatch";
            return false;
        }

        if (const SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(impl_->ssl.get())) {
            impl_->selectedSrtpProfile = profile->name != nullptr ? profile->name : "";
        } else {
            impl_->lastError = "DTLS SRTP profile was not negotiated";
            return false;
        }
    } else {
        const int error = SSL_get_error(impl_->ssl.get(), ret);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            impl_->lastError = CurrentSslError("SSL_do_handshake");
            return false;
        }
    }

    return DrainBio(impl_->writeBio, outgoingPackets, &impl_->lastError);
}

bool DtlsTransport::IsStarted() const noexcept {
    return impl_ && impl_->started;
}

bool DtlsTransport::IsConnected() const noexcept {
    return impl_ && impl_->connected;
}

std::string DtlsTransport::SelectedSrtpProfile() const {
    return impl_ ? impl_->selectedSrtpProfile : std::string{};
}

std::string DtlsTransport::PeerFingerprintSha256() const {
    return impl_ ? impl_->peerFingerprint : std::string{};
}

const std::string& DtlsTransport::LastError() const noexcept {
    static const std::string kEmpty;
    return impl_ ? impl_->lastError : kEmpty;
}

bool DtlsTransport::ExportSrtpKeyMaterial(std::size_t keyLen,
                                          std::size_t saltLen,
                                          SrtpKeyMaterial* out) const {
    if (!impl_ || !impl_->connected || impl_->ssl == nullptr || out == nullptr || keyLen == 0U || saltLen == 0U) {
        return false;
    }

    const std::size_t totalLen = (keyLen + saltLen) * 2U;
    std::vector<uint8_t> exporter(totalLen, 0U);
    static constexpr char kExporterLabel[] = "EXTRACTOR-dtls_srtp";
    if (SSL_export_keying_material(impl_->ssl.get(),
                                   exporter.data(),
                                   exporter.size(),
                                   kExporterLabel,
                                   sizeof(kExporterLabel) - 1U,
                                   nullptr,
                                   0,
                                   0) != 1) {
        return false;
    }

    const uint8_t* cursor = exporter.data();
    const uint8_t* clientKey = cursor;
    cursor += keyLen;
    const uint8_t* serverKey = cursor;
    cursor += keyLen;
    const uint8_t* clientSalt = cursor;
    cursor += saltLen;
    const uint8_t* serverSalt = cursor;

    const bool localIsClient = impl_->role == Role::Client;
    const uint8_t* localKey = localIsClient ? clientKey : serverKey;
    const uint8_t* remoteKey = localIsClient ? serverKey : clientKey;
    const uint8_t* localSalt = localIsClient ? clientSalt : serverSalt;
    const uint8_t* remoteSalt = localIsClient ? serverSalt : clientSalt;

    out->localKey.assign(localKey, localKey + keyLen);
    out->remoteKey.assign(remoteKey, remoteKey + keyLen);
    out->localSalt.assign(localSalt, localSalt + saltLen);
    out->remoteSalt.assign(remoteSalt, remoteSalt + saltLen);
    return true;
}

bool DtlsTransport::LooksLikeDtlsRecord(const uint8_t* data, std::size_t len) {
    if (data == nullptr || len < 13U) {
        return false;
    }

    switch (data[0]) {
    case 20U:
    case 21U:
    case 22U:
    case 23U:
    case 24U:
        break;
    default:
        return false;
    }

    return data[1] == 0xFEU && (data[2] == 0xFFU || data[2] == 0xFDU || data[2] == 0xFCU);
}

} // namespace sfu
