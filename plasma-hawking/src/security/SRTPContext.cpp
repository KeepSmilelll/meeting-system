#include "SRTPContext.h"

#include "CryptoUtils.h"

namespace security {

bool SRTPContext::configure(const QByteArray& masterKey, const QByteArray& masterSalt) {
    if (masterKey.isEmpty() || masterSalt.isEmpty()) {
        return false;
    }

    m_masterKey = masterKey;
    m_masterSalt = masterSalt;
    return true;
}

void SRTPContext::clear() {
    m_masterKey.fill('\0');
    m_masterSalt.fill('\0');
    m_masterKey.clear();
    m_masterSalt.clear();
}

bool SRTPContext::configured() const {
    return !m_masterKey.isEmpty() && !m_masterSalt.isEmpty();
}

QString SRTPContext::keyFingerprint() const {
    if (!configured()) {
        return QString();
    }

    return CryptoUtils::sha256Hex(m_masterKey + m_masterSalt).left(16);
}

}  // namespace security
