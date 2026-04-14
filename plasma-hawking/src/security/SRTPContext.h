#pragma once

#include <QByteArray>
#include <QString>

namespace security {

class SRTPContext {
public:
    bool configure(const QByteArray& masterKey, const QByteArray& masterSalt);
    void clear();

    bool configured() const;
    QString keyFingerprint() const;

private:
    QByteArray m_masterKey;
    QByteArray m_masterSalt;
};

}  // namespace security
