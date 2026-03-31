#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QVariantMap>

class UserRepository {
public:
    explicit UserRepository(const QString& databasePath = QString());
    ~UserRepository();

    bool isOpen() const;

    bool upsertUser(const QString& userId,
                    const QString& username,
                    const QString& displayName = QString(),
                    const QString& avatarUrl = QString(),
                    int status = 0,
                    const QString& token = QString(),
                    qint64 lastSeenAt = 0);
    QVariantMap findById(const QString& userId) const;
    bool setStatus(const QString& userId, int status);
    bool saveToken(const QString& userId, const QString& token);
    bool clearToken(const QString& userId);

private:
    bool ensureOpen();
    bool open();

    QString m_databasePath;
    QString m_connectionName;
    QSqlDatabase m_db;
};
