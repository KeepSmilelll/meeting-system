#include <cassert>
#include <QCoreApplication>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QStringList>

#include "../src/storage/DatabaseManager.h"
#include "../src/app/ChatMessageListModel.h"
#include "../src/storage/MeetingRepository.h"
#include "../src/storage/MessageRepository.h"
#include "../src/storage/SettingsRepository.h"

namespace {

bool tableExists(QSqlDatabase db, const QString& name) {
    QSqlQuery query(db);
    query.prepare(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type IN ('table','view','virtual table') AND name = ? LIMIT 1"));
    query.addBindValue(name);
    return query.exec() && query.next();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tempDir;
    assert(tempDir.isValid());
    const QString databasePath = tempDir.filePath(QStringLiteral("phase2.sqlite"));

    QSqlDatabase db = DatabaseManager::instance().openDatabase(QStringLiteral("test_database_connection"), databasePath);
    assert(db.isValid());
    assert(db.isOpen());

    QSqlQuery pragmaQuery(db);
    assert(pragmaQuery.exec(QStringLiteral("PRAGMA journal_mode")));
    assert(pragmaQuery.next());
    assert(pragmaQuery.value(0).toString().compare(QStringLiteral("wal"), Qt::CaseInsensitive) == 0);

    assert(pragmaQuery.exec(QStringLiteral("PRAGMA foreign_keys")));
    assert(pragmaQuery.next());
    assert(pragmaQuery.value(0).toInt() == 1);

    assert(pragmaQuery.exec(QStringLiteral("PRAGMA busy_timeout")));
    assert(pragmaQuery.next());
    assert(pragmaQuery.value(0).toInt() == 5000);

    const QStringList requiredTables = DatabaseManager::instance().requiredTables();
    for (const QString& table : requiredTables) {
        assert(tableExists(db, table));
    }

    SettingsRepository settings(databasePath);
    assert(settings.isOpen());
    assert(settings.saveToken(QStringLiteral("token-123")));
    assert(settings.token() == QStringLiteral("token-123"));
    assert(settings.saveServerEndpoint(QStringLiteral("127.0.0.1"), 8443));
    assert(settings.serverHost() == QStringLiteral("127.0.0.1"));
    assert(settings.serverPort() == 8443);
    assert(settings.saveIcePolicy(QStringLiteral("relay-only")));
    assert(settings.icePolicy() == QStringLiteral("relay-only"));
    assert(settings.savePreferredCameraDevice(QStringLiteral("e2eSoft iVCam")));
    assert(settings.preferredCameraDevice() == QStringLiteral("e2eSoft iVCam"));

    MeetingRepository meetings(databasePath);
    assert(meetings.isOpen());
    assert(meetings.upsertMeeting(QStringLiteral("123456"), QStringLiteral("Daily Sync"), QStringLiteral("host-1"), 1000));
    assert(meetings.markMeetingLeft(QStringLiteral("123456"), 2000));

    MessageRepository messages(databasePath);
    assert(messages.isOpen());
    assert(messages.saveMessage(QStringLiteral("123456"),
                                QStringLiteral("user-1"),
                                QStringLiteral("Alice"),
                                QStringLiteral("hello sqlite fts"),
                                3000,
                                0,
                                QString(),
                                false,
                                QStringLiteral("remote-1")));
    assert(messages.saveMessage(QStringLiteral("123456"),
                                QStringLiteral("user-1"),
                                QStringLiteral("Alice"),
                                QStringLiteral("hello sqlite fts"),
                                3000,
                                0,
                                QString(),
                                false,
                                QStringLiteral("remote-1")));
    const auto meetingMessages = messages.listByMeeting(QStringLiteral("123456"));
    assert(meetingMessages.size() == 1);
    assert(meetingMessages.first().remoteMessageId == QStringLiteral("remote-1"));
    const auto searchResults = messages.searchMessages(QStringLiteral("123456"), QStringLiteral("sqlite"));
    assert(!searchResults.isEmpty());
    assert(searchResults.first().content.contains(QStringLiteral("sqlite"), Qt::CaseInsensitive));

    for (int i = 0; i < 55; ++i) {
        assert(messages.saveMessage(QStringLiteral("123456"),
                                    QStringLiteral("user-2"),
                                    QStringLiteral("Bob"),
                                    QStringLiteral("paged message %1").arg(i, 2, 10, QLatin1Char('0')),
                                    4000 + i,
                                    0,
                                    QString(),
                                    false,
                                    QStringLiteral("page-%1").arg(i, 2, 10, QLatin1Char('0'))));
    }
    const auto latestPage = messages.listByMeeting(QStringLiteral("123456"), 50);
    assert(latestPage.size() == 50);
    assert(latestPage.first().remoteMessageId == QStringLiteral("page-05"));
    assert(latestPage.last().remoteMessageId == QStringLiteral("page-54"));
    const auto olderPage = messages.listByMeeting(QStringLiteral("123456"), 50, latestPage.first().sentAt);
    assert(olderPage.size() == 6);
    assert(olderPage.first().remoteMessageId == QStringLiteral("remote-1"));
    assert(olderPage.last().remoteMessageId == QStringLiteral("page-04"));

    ChatMessageListModel model;
    assert(model.appendMessages(latestPage) == 50);
    assert(model.rowCount() == 50);
    assert(model.oldestMessageTimestamp() == latestPage.first().sentAt);
    assert(model.prependMessages(olderPage) == 6);
    assert(model.rowCount() == 56);
    assert(model.prependMessages(olderPage) == 0);
    assert(model.rowCount() == 56);

    db.close();
    return 0;
}



