#include "DatabaseManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlQuery>
#include <QtGlobal>
#include <QStandardPaths>

namespace {

bool execSql(QSqlQuery& query, const QString& sql) {
    return query.exec(sql);
}

}  // namespace

DatabaseManager& DatabaseManager::instance() {
    static DatabaseManager manager;
    return manager;
}

QString DatabaseManager::defaultDatabasePath() {
    QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDir.isEmpty()) {
        baseDir = QDir::homePath() + QStringLiteral("/.meeting-client");
    }

    QDir dir(baseDir);
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }
    return dir.filePath(QStringLiteral("settings.sqlite"));
}

QSqlDatabase DatabaseManager::openDatabase(const QString& connectionName, const QString& databasePath) const {
    const QString resolvedPath = resolvedDatabasePath(databasePath);
    if (!ensureParentDirectory(resolvedPath)) {
        return {};
    }

    const QString connection = createConnectionName(connectionName);
    if (QSqlDatabase::contains(connection)) {
        QSqlDatabase existing = QSqlDatabase::database(connection, false);
        if (existing.isValid() && existing.isOpen()) {
            return existing;
        }
        QSqlDatabase::removeDatabase(connection);
    }

    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
    db.setDatabaseName(resolvedPath);
    if (!db.open()) {
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return {};
    }

    if (!applyPragmas(db) || !createSchema(db)) {
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connection);
        return {};
    }

    return db;
}

QStringList DatabaseManager::requiredTables() const {
    return {
        QStringLiteral("settings"),
        QStringLiteral("user"),
        QStringLiteral("meeting"),
        QStringLiteral("meeting_participant"),
        QStringLiteral("message"),
        QStringLiteral("contact"),
        QStringLiteral("file_transfer"),
        QStringLiteral("call_log"),
        QStringLiteral("message_fts"),
    };
}

bool DatabaseManager::applyPragmas(QSqlDatabase& db) const {
    QSqlQuery pragmaQuery(db);
    return execSql(pragmaQuery, QStringLiteral("PRAGMA journal_mode=WAL")) &&
           execSql(pragmaQuery, QStringLiteral("PRAGMA foreign_keys=ON")) &&
           execSql(pragmaQuery, QStringLiteral("PRAGMA busy_timeout=5000"));
}

bool DatabaseManager::createSchema(QSqlDatabase& db) const {
    QSqlQuery query(db);

    const QStringList schemaStatements{
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS settings ("
            "category TEXT NOT NULL DEFAULT '',"
            "key TEXT NOT NULL,"
            "value TEXT NOT NULL,"
            "updated_at INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(category, key)"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS user ("
            "user_id TEXT PRIMARY KEY,"
            "username TEXT NOT NULL DEFAULT '',"
            "display_name TEXT NOT NULL DEFAULT '',"
            "avatar_url TEXT NOT NULL DEFAULT '',"
            "status INTEGER NOT NULL DEFAULT 0,"
            "token TEXT NOT NULL DEFAULT '',"
            "last_seen_at INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS meeting ("
            "meeting_id TEXT PRIMARY KEY,"
            "title TEXT NOT NULL DEFAULT '',"
            "host_user_id TEXT NOT NULL DEFAULT '',"
            "last_joined_at INTEGER NOT NULL DEFAULT 0,"
            "last_left_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS meeting_participant ("
            "meeting_id TEXT NOT NULL,"
            "user_id TEXT NOT NULL,"
            "display_name TEXT NOT NULL DEFAULT '',"
            "avatar_url TEXT NOT NULL DEFAULT '',"
            "role INTEGER NOT NULL DEFAULT 0,"
            "is_audio_on INTEGER NOT NULL DEFAULT 1,"
            "is_video_on INTEGER NOT NULL DEFAULT 1,"
            "is_sharing INTEGER NOT NULL DEFAULT 0,"
            "joined_at INTEGER NOT NULL DEFAULT 0,"
            "left_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0,"
            "PRIMARY KEY(meeting_id, user_id),"
            "FOREIGN KEY(meeting_id) REFERENCES meeting(meeting_id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS message ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "meeting_id TEXT NOT NULL,"
            "sender_id TEXT NOT NULL DEFAULT '',"
            "sender_name TEXT NOT NULL DEFAULT '',"
            "content TEXT NOT NULL DEFAULT '',"
            "message_type INTEGER NOT NULL DEFAULT 0,"
            "reply_to_id TEXT NOT NULL DEFAULT '',"
            "sent_at INTEGER NOT NULL DEFAULT 0,"
            "is_local INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "FOREIGN KEY(meeting_id) REFERENCES meeting(meeting_id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS contact ("
            "user_id TEXT PRIMARY KEY,"
            "display_name TEXT NOT NULL DEFAULT '',"
            "avatar_url TEXT NOT NULL DEFAULT '',"
            "remark TEXT NOT NULL DEFAULT '',"
            "is_blocked INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS file_transfer ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "transfer_id TEXT NOT NULL UNIQUE,"
            "meeting_id TEXT NOT NULL DEFAULT '',"
            "file_name TEXT NOT NULL DEFAULT '',"
            "file_size INTEGER NOT NULL DEFAULT 0,"
            "transferred_size INTEGER NOT NULL DEFAULT 0,"
            "sha256 TEXT NOT NULL DEFAULT '',"
            "state INTEGER NOT NULL DEFAULT 0,"
            "direction INTEGER NOT NULL DEFAULT 0,"
            "created_at INTEGER NOT NULL DEFAULT 0,"
            "updated_at INTEGER NOT NULL DEFAULT 0,"
            "FOREIGN KEY(meeting_id) REFERENCES meeting(meeting_id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE TABLE IF NOT EXISTS call_log ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "meeting_id TEXT NOT NULL,"
            "title TEXT NOT NULL DEFAULT '',"
            "user_id TEXT NOT NULL,"
            "joined_at INTEGER NOT NULL DEFAULT 0,"
            "left_at INTEGER,"
            "leave_reason TEXT NOT NULL DEFAULT '',"
            "was_host INTEGER NOT NULL DEFAULT 0,"
            "FOREIGN KEY(meeting_id) REFERENCES meeting(meeting_id) ON DELETE CASCADE"
            ")"),
        QStringLiteral(
            "CREATE VIRTUAL TABLE IF NOT EXISTS message_fts USING fts5("
            "message_id UNINDEXED,"
            "meeting_id UNINDEXED,"
            "sender_id UNINDEXED,"
            "sender_name,"
            "content,"
            "tokenize='unicode61'"
            ")"),
    };

    for (const QString& statement : schemaStatements) {
        if (!execSql(query, statement)) {
            return false;
        }
    }

    const QStringList indexStatements{
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_meeting_updated_at ON meeting(updated_at DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_meeting_participant_meeting ON meeting_participant(meeting_id, joined_at ASC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_message_meeting_sent_at ON message(meeting_id, sent_at DESC, id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_call_log_meeting_user ON call_log(meeting_id, user_id, joined_at DESC, id DESC)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_contact_display_name ON contact(display_name)"),
    };

    for (const QString& statement : indexStatements) {
        if (!execSql(query, statement)) {
            return false;
        }
    }

    const QStringList triggerStatements{
        QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS message_ai AFTER INSERT ON message BEGIN "
            "INSERT INTO message_fts(rowid, message_id, meeting_id, sender_id, sender_name, content) "
            "VALUES (new.id, new.id, new.meeting_id, new.sender_id, new.sender_name, new.content); "
            "END;"),
        QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS message_ad AFTER DELETE ON message BEGIN "
            "INSERT INTO message_fts(message_fts, rowid, message_id, meeting_id, sender_id, sender_name, content) "
            "VALUES('delete', old.id, old.id, old.meeting_id, old.sender_id, old.sender_name, old.content); "
            "END;"),
        QStringLiteral(
            "CREATE TRIGGER IF NOT EXISTS message_au AFTER UPDATE ON message BEGIN "
            "INSERT INTO message_fts(message_fts, rowid, message_id, meeting_id, sender_id, sender_name, content) "
            "VALUES('delete', old.id, old.id, old.meeting_id, old.sender_id, old.sender_name, old.content); "
            "INSERT INTO message_fts(rowid, message_id, meeting_id, sender_id, sender_name, content) "
            "VALUES (new.id, new.id, new.meeting_id, new.sender_id, new.sender_name, new.content); "
            "END;"),
    };

    for (const QString& statement : triggerStatements) {
        if (!execSql(query, statement)) {
            return false;
        }
    }

    return true;
}

bool DatabaseManager::ensureParentDirectory(const QString& databasePath) {
    const QFileInfo info(databasePath);
    QDir parentDir(info.absolutePath());
    if (!parentDir.exists()) {
        return parentDir.mkpath(QStringLiteral("."));
    }
    return true;
}

QString DatabaseManager::resolvedDatabasePath(const QString& databasePath) const {
    return databasePath.isEmpty() ? defaultDatabasePath() : databasePath;
}

QString DatabaseManager::createConnectionName(const QString& connectionName) const {
    if (!connectionName.trimmed().isEmpty()) {
        return connectionName;
    }

    return QStringLiteral("database_manager_%1")
        .arg(QString::number(reinterpret_cast<quintptr>(this), 16));
}




