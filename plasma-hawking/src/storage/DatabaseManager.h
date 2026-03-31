#pragma once

#include <QSqlDatabase>
#include <QString>
#include <QStringList>

class DatabaseManager {
public:
    static DatabaseManager& instance();

    static QString defaultDatabasePath();

    QSqlDatabase openDatabase(const QString& connectionName,
                              const QString& databasePath = QString()) const;

    QStringList requiredTables() const;

private:
    DatabaseManager() = default;

    bool applyPragmas(QSqlDatabase& db) const;
    bool createSchema(QSqlDatabase& db) const;
    static bool ensureParentDirectory(const QString& databasePath);

    QString resolvedDatabasePath(const QString& databasePath) const;
    QString createConnectionName(const QString& connectionName) const;
};
