#include "DtlsTransportClient.h"

#include <QList>

#include <array>
#include <cctype>
#include <iomanip>
#include <memory>
#include <sstream>
#include <utility>

#include <openssl/asn1.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace media {
namespace {

struct SslCtxDeleter {
    void operator()(SSL_CTX* ctx) const noexcept { SSL_CTX_free(ctx); }
};

struct SslDeleter {
    void operator()(SSL* ssl) const noexcept { SSL_free(ssl); }
};

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};

struct X509Deleter {
    void operator()(X509* cert) const noexcept { X509_free(cert); }
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;
using SslPtr = std::unique_ptr<SSL, SslDeleter>;
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

EvpPkeyPtr generateKey() {
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

bool setRandomSerial(X509* cert) {
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

X509Ptr generateCertificate(EVP_PKEY* key) {
    X509Ptr cert(X509_new());
    if (!cert || key == nullptr) {
        return {};
    }

    if (X509_set_version(cert.get(), 2) != 1 ||
        !setRandomSerial(cert.get()) ||
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
                                   reinterpret_cast<const unsigned char*>("meeting-client"),
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

QString formatFingerprint(const unsigned char* digest, unsigned int digestLen) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < digestLen; ++i) {
        if (i != 0U) {
            stream << ':';
        }
        stream << std::setw(2) << static_cast<unsigned int>(digest[i]);
    }
    return QString::fromStdString(stream.str());
}

QString normalizeFingerprint(const QString& fingerprint) {
    QString normalized;
    normalized.reserve(fingerprint.size());
    for (const QChar ch : fingerprint) {
        if (ch == QLatin1Char(':') || ch.isDigit() ||
            (ch.toUpper() >= QLatin1Char('A') && ch.toUpper() <= QLatin1Char('F'))) {
            normalized.append(ch.toUpper());
        }
    }
    return normalized;
}

QString currentSslError(const char* prefix) {
    const unsigned long code = ERR_get_error();
    if (code == 0UL) {
        return QStringLiteral("%1: unknown OpenSSL error").arg(QString::fromLatin1(prefix));
    }

    std::array<char, 256> buffer{};
    ERR_error_string_n(code, buffer.data(), buffer.size());
    return QStringLiteral("%1: %2")
        .arg(QString::fromLatin1(prefix), QString::fromLatin1(buffer.data()));
}

int allowPinnedPeerCertificate(int, X509_STORE_CTX*) {
    return 1;
}

bool drainBio(BIO* bio, QList<QByteArray>* outgoingPackets, QString* error) {
    if (bio == nullptr || outgoingPackets == nullptr) {
        return true;
    }

    while (BIO_ctrl_pending(bio) > 0) {
        const long pending = BIO_ctrl_pending(bio);
        if (pending <= 0) {
            break;
        }
        QByteArray packet(static_cast<int>(pending), Qt::Uninitialized);
        const int read = BIO_read(bio, packet.data(), packet.size());
        if (read <= 0) {
            if (error != nullptr) {
                *error = QStringLiteral("DTLS write BIO drain failed");
            }
            return false;
        }
        packet.resize(read);
        outgoingPackets->append(std::move(packet));
    }
    return true;
}

bool exportPeerFingerprint(SSL* ssl, QString* fingerprint) {
    if (ssl == nullptr || fingerprint == nullptr) {
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
    *fingerprint = formatFingerprint(digest, digestLen);
    return true;
}

}  // namespace

struct DtlsTransportClient::Impl {
    SslCtxPtr sslContext;
    SslPtr ssl;
    EvpPkeyPtr key;
    X509Ptr cert;
    BIO* readBio{nullptr};
    BIO* writeBio{nullptr};
    bool started{false};
    bool connected{false};
    QString expectedServerFingerprint;
    QString localFingerprint;
    QString peerFingerprint;
    QString selectedSrtpProfile;
    QString lastError;
};

template <typename ImplT>
bool ensureLocalIdentity(ImplT* impl) {
    if (impl == nullptr) {
        return false;
    }
    if (impl->key && impl->cert && !impl->localFingerprint.isEmpty()) {
        return true;
    }

    OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS |
                         OPENSSL_INIT_LOAD_CRYPTO_STRINGS |
                         OPENSSL_INIT_NO_LOAD_CONFIG,
                     nullptr);

    auto key = generateKey();
    auto cert = generateCertificate(key.get());
    if (!key || !cert) {
        impl->lastError = QStringLiteral("DTLS certificate generation failed");
        return false;
    }

    unsigned char digest[EVP_MAX_MD_SIZE]{};
    unsigned int digestLen = 0;
    if (X509_digest(cert.get(), EVP_sha256(), digest, &digestLen) != 1 || digestLen == 0U) {
        impl->lastError = QStringLiteral("DTLS fingerprint export failed");
        return false;
    }

    impl->key = std::move(key);
    impl->cert = std::move(cert);
    impl->localFingerprint = formatFingerprint(digest, digestLen);
    impl->lastError.clear();
    return true;
}

DtlsTransportClient::DtlsTransportClient()
    : m_impl(std::make_unique<Impl>()) {}

DtlsTransportClient::~DtlsTransportClient() = default;

DtlsTransportClient::DtlsTransportClient(DtlsTransportClient&&) noexcept = default;

DtlsTransportClient& DtlsTransportClient::operator=(DtlsTransportClient&&) noexcept = default;

bool DtlsTransportClient::prepareLocalFingerprint() {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    return ensureLocalIdentity(m_impl.get());
}

bool DtlsTransportClient::start(const QString& expectedServerFingerprint,
                                QList<QByteArray>* outgoingPackets) {
    if (!m_impl) {
        m_impl = std::make_unique<Impl>();
    }
    if (m_impl->started) {
        return true;
    }

    if (!ensureLocalIdentity(m_impl.get())) {
        return false;
    }

    SslCtxPtr sslContext(SSL_CTX_new(DTLS_method()));
    if (!sslContext) {
        m_impl->lastError = currentSslError("SSL_CTX_new");
        return false;
    }
    SSL_CTX_set_min_proto_version(sslContext.get(), DTLS1_2_VERSION);
    SSL_CTX_set_max_proto_version(sslContext.get(), DTLS1_2_VERSION);
    SSL_CTX_set_verify(sslContext.get(), SSL_VERIFY_PEER, allowPinnedPeerCertificate);
    SSL_CTX_set_read_ahead(sslContext.get(), 1);
    if (SSL_CTX_use_certificate(sslContext.get(), m_impl->cert.get()) != 1 ||
        SSL_CTX_use_PrivateKey(sslContext.get(), m_impl->key.get()) != 1 ||
        SSL_CTX_check_private_key(sslContext.get()) != 1 ||
        SSL_CTX_set_tlsext_use_srtp(sslContext.get(), "SRTP_AES128_CM_SHA1_80") != 0) {
        m_impl->lastError = currentSslError("SSL_CTX configure");
        return false;
    }

    SSL* rawSsl = SSL_new(sslContext.get());
    if (rawSsl == nullptr) {
        m_impl->lastError = currentSslError("SSL_new");
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
        m_impl->lastError = QStringLiteral("BIO_new failed");
        return false;
    }

    BIO_set_mem_eof_return(readBio, -1);
    BIO_set_mem_eof_return(writeBio, -1);
    SSL_set_bio(rawSsl, readBio, writeBio);
    SSL_set_mtu(rawSsl, 1200);
    SSL_set_connect_state(rawSsl);

    m_impl->sslContext = std::move(sslContext);
    m_impl->ssl.reset(rawSsl);
    m_impl->readBio = readBio;
    m_impl->writeBio = writeBio;
    m_impl->expectedServerFingerprint = normalizeFingerprint(expectedServerFingerprint);
    m_impl->started = true;
    m_impl->lastError.clear();

    const int ret = SSL_do_handshake(m_impl->ssl.get());
    if (ret != 1) {
        const int error = SSL_get_error(m_impl->ssl.get(), ret);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            m_impl->lastError = currentSslError("SSL_do_handshake");
            return false;
        }
    }

    return drainBio(m_impl->writeBio, outgoingPackets, &m_impl->lastError);
}

bool DtlsTransportClient::handleIncomingDatagram(const QByteArray& datagram,
                                                 QList<QByteArray>* outgoingPackets) {
    if (!m_impl || !m_impl->started || !m_impl->ssl || datagram.isEmpty()) {
        if (m_impl) {
            m_impl->lastError = QStringLiteral("DTLS client is not ready for incoming datagrams");
        }
        return false;
    }

    if (BIO_write(m_impl->readBio, datagram.constData(), datagram.size()) != datagram.size()) {
        m_impl->lastError = QStringLiteral("BIO_write failed for DTLS datagram");
        return false;
    }

    const int ret = SSL_do_handshake(m_impl->ssl.get());
    if (ret == 1) {
        m_impl->connected = true;
        if (!exportPeerFingerprint(m_impl->ssl.get(), &m_impl->peerFingerprint)) {
            m_impl->lastError = QStringLiteral("DTLS server fingerprint export failed");
            return false;
        }
        if (!m_impl->expectedServerFingerprint.isEmpty() &&
            normalizeFingerprint(m_impl->peerFingerprint) != m_impl->expectedServerFingerprint) {
            m_impl->lastError = QStringLiteral("DTLS server fingerprint mismatch");
            return false;
        }
        if (const SRTP_PROTECTION_PROFILE* profile = SSL_get_selected_srtp_profile(m_impl->ssl.get())) {
            m_impl->selectedSrtpProfile = QString::fromLatin1(profile->name);
        } else {
            m_impl->lastError = QStringLiteral("DTLS SRTP profile was not negotiated");
            return false;
        }
    } else {
        const int error = SSL_get_error(m_impl->ssl.get(), ret);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE) {
            m_impl->lastError = currentSslError("SSL_do_handshake");
            return false;
        }
    }

    return drainBio(m_impl->writeBio, outgoingPackets, &m_impl->lastError);
}

bool DtlsTransportClient::isStarted() const {
    return m_impl && m_impl->started;
}

bool DtlsTransportClient::isConnected() const {
    return m_impl && m_impl->connected;
}

QString DtlsTransportClient::localFingerprintSha256() const {
    return m_impl ? m_impl->localFingerprint : QString{};
}

QString DtlsTransportClient::peerFingerprintSha256() const {
    return m_impl ? m_impl->peerFingerprint : QString{};
}

QString DtlsTransportClient::selectedSrtpProfile() const {
    return m_impl ? m_impl->selectedSrtpProfile : QString{};
}

QString DtlsTransportClient::lastError() const {
    return m_impl ? m_impl->lastError : QString{};
}

bool DtlsTransportClient::exportSrtpKeyMaterial(int keyLen, int saltLen, SrtpKeyMaterial* out) const {
    if (!m_impl || !m_impl->connected || !m_impl->ssl || out == nullptr || keyLen <= 0 || saltLen <= 0) {
        return false;
    }

    const int totalLen = (keyLen + saltLen) * 2;
    QByteArray exporter(totalLen, Qt::Uninitialized);
    static constexpr char kExporterLabel[] = "EXTRACTOR-dtls_srtp";
    if (SSL_export_keying_material(m_impl->ssl.get(),
                                   reinterpret_cast<unsigned char*>(exporter.data()),
                                   exporter.size(),
                                   kExporterLabel,
                                   sizeof(kExporterLabel) - 1U,
                                   nullptr,
                                   0,
                                   0) != 1) {
        return false;
    }

    const char* base = exporter.constData();
    const QByteArray clientKey(base, keyLen);
    const QByteArray serverKey(base + keyLen, keyLen);
    const QByteArray clientSalt(base + (keyLen * 2), saltLen);
    const QByteArray serverSalt(base + (keyLen * 2) + saltLen, saltLen);

    out->localKey = clientKey;
    out->remoteKey = serverKey;
    out->localSalt = clientSalt;
    out->remoteSalt = serverSalt;
    return true;
}

bool DtlsTransportClient::looksLikeDtlsRecord(const QByteArray& datagram) {
    if (datagram.size() < 13) {
        return false;
    }

    const unsigned char* data =
        reinterpret_cast<const unsigned char*>(datagram.constData());
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

}  // namespace media
