#include "server/DtlsContext.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace sfu {
namespace {

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept {
        EVP_PKEY_free(key);
    }
};

struct X509Deleter {
    void operator()(X509* cert) const noexcept {
        X509_free(cert);
    }
};

using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept {
        SSL_CTX_free(ctx);
    }
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

EvpPkeyPtr GenerateKey() {
    EVP_PKEY_CTX* rawCtx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (rawCtx == nullptr) {
        return {};
    }
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(rawCtx, EVP_PKEY_CTX_free);
    if (EVP_PKEY_keygen_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0) {
        return {};
    }

    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen(ctx.get(), &rawKey) <= 0) {
        return {};
    }
    return EvpPkeyPtr(rawKey);
}

bool SetRandomSerial(X509* cert) {
    std::array<unsigned char, 16> serialBytes{};
    if (RAND_bytes(serialBytes.data(), static_cast<int>(serialBytes.size())) != 1) {
        return false;
    }
    serialBytes[0] &= 0x7FU;

    BIGNUM* rawBn = BN_bin2bn(serialBytes.data(), static_cast<int>(serialBytes.size()), nullptr);
    if (rawBn == nullptr) {
        return false;
    }
    std::unique_ptr<BIGNUM, decltype(&BN_free)> bn(rawBn, BN_free);

    ASN1_INTEGER* rawSerial = BN_to_ASN1_INTEGER(bn.get(), nullptr);
    if (rawSerial == nullptr) {
        return false;
    }
    std::unique_ptr<ASN1_INTEGER, decltype(&ASN1_INTEGER_free)> serial(rawSerial, ASN1_INTEGER_free);
    return X509_set_serialNumber(cert, serial.get()) == 1;
}

X509Ptr GenerateCertificate(EVP_PKEY* key) {
    X509Ptr cert(X509_new());
    if (!cert || key == nullptr) {
        return {};
    }

    if (X509_set_version(cert.get(), 2) != 1 ||
        !SetRandomSerial(cert.get()) ||
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L * 24L * 365L) == nullptr ||
        X509_set_pubkey(cert.get(), key) != 1) {
        return {};
    }

    X509_NAME* name = X509_get_subject_name(cert.get());
    if (name == nullptr ||
        X509_NAME_add_entry_by_txt(name,
                                   "CN",
                                   MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("meeting-sfu"),
                                   -1,
                                   -1,
                                   0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1) {
        return {};
    }

    if (X509_sign(cert.get(), key, EVP_sha256()) <= 0) {
        return {};
    }
    return cert;
}

std::string FormatFingerprint(const unsigned char* digest, unsigned int digestLen) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        if (i != 0) {
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

int AllowPinnedPeerCertificate(int, X509_STORE_CTX*) {
    return 1;
}

} // namespace

struct DtlsContext::Impl {
    SslCtxPtr sslContext;
    EvpPkeyPtr key;
    X509Ptr cert;
    std::string fingerprint;
    std::string lastError;
};

DtlsContext::DtlsContext()
    : impl_(std::make_unique<Impl>()) {}

DtlsContext::~DtlsContext() = default;

DtlsContext::DtlsContext(DtlsContext&&) noexcept = default;

DtlsContext& DtlsContext::operator=(DtlsContext&&) noexcept = default;

bool DtlsContext::Initialize() {
    if (IsReady()) {
        return true;
    }
    impl_->lastError.clear();

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                         OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                         OPENSSL_INIT_NO_LOAD_CONFIG,
                     nullptr);

    auto key = GenerateKey();
    auto cert = GenerateCertificate(key.get());
    if (!key || !cert) {
        impl_->lastError = "DTLS key/certificate generation failed";
        return false;
    }

    SslCtxPtr sslContext(SSL_CTX_new(DTLS_method()));
    if (!sslContext) {
        impl_->lastError = CurrentSslError("SSL_CTX_new");
        return false;
    }

    SSL_CTX_set_min_proto_version(sslContext.get(), DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(sslContext.get(), DTLS1_2_VERSION);
    SSL_CTX_set_verify(sslContext.get(),
                       SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE,
                       AllowPinnedPeerCertificate);
    SSL_CTX_set_read_ahead(sslContext.get(), 1);
    if (SSL_CTX_use_certificate(sslContext.get(), cert.get()) != 1) {
        impl_->lastError = CurrentSslError("SSL_CTX_use_certificate");
        return false;
    }
    if (SSL_CTX_use_PrivateKey(sslContext.get(), key.get()) != 1) {
        impl_->lastError = CurrentSslError("SSL_CTX_use_PrivateKey");
        return false;
    }
    if (SSL_CTX_check_private_key(sslContext.get()) != 1) {
        impl_->lastError = CurrentSslError("SSL_CTX_check_private_key");
        return false;
    }
    if (SSL_CTX_set_tlsext_use_srtp(sslContext.get(), "SRTP_AES128_CM_SHA1_80") != 0) {
        impl_->lastError = CurrentSslError("SSL_CTX_set_tlsext_use_srtp");
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digestLen = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &digestLen) != 1 || digestLen == 0) {
        impl_->lastError = CurrentSslError("X509_digest");
        return false;
    }

    impl_->fingerprint = FormatFingerprint(digest, digestLen);
    impl_->sslContext = std::move(sslContext);
    impl_->key = std::move(key);
    impl_->cert = std::move(cert);
    return true;
}

bool DtlsContext::IsReady() const noexcept {
    return impl_ && impl_->sslContext && impl_->key && impl_->cert && !impl_->fingerprint.empty();
}

std::string DtlsContext::FingerprintSha256() const {
    return impl_ ? impl_->fingerprint : std::string{};
}

std::string DtlsContext::LastError() const {
    return impl_ ? impl_->lastError : std::string{};
}

SSL_CTX* DtlsContext::RawContext() const noexcept {
    return impl_ ? impl_->sslContext.get() : nullptr;
}

} // namespace sfu

