#include <cassert>
#include <functional>
#include <string>

#include <QByteArray>
#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QThread>
#include <QStringList>

#include "app/MeetingController.h"
#include "av/render/VideoFrameStore.h"
#include "net/signaling/SignalingClient.h"
#include "signaling.pb.h"

namespace {

constexpr quint16 kMeetMuteAll = 0x0209;
constexpr quint16 kMeetStateSync = 0x020B;
constexpr quint16 kMediaRouteStatusNotify = 0x0306;

bool emitProtoMessage(signaling::SignalingClient* signalingClient, quint16 signalType, const google::protobuf::MessageLite& message) {
    assert(signalingClient != nullptr);

    std::string serialized;
    if (!message.SerializeToString(&serialized)) {
        return false;
    }

    return QMetaObject::invokeMethod(signalingClient,
                                     "protobufMessageReceived",
                                     Qt::DirectConnection,
                                     Q_ARG(quint16, signalType),
                                     Q_ARG(QByteArray, QByteArray(serialized.data(), static_cast<int>(serialized.size()))));
}

bool containsAnyMessage(const QStringList& messages, std::initializer_list<QString> needles) {
    for (const QString& message : messages) {
        for (const QString& needle : needles) {
            if (message == needle) {
                return true;
            }
        }
    }
    return false;
}

int roleForName(const QAbstractItemModel* model, const QByteArray& roleName) {
    assert(model != nullptr);
    const auto roles = model->roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
        if (it.value() == roleName) {
            return it.key();
        }
    }
    return -1;
}

QVariant participantData(const QAbstractItemModel* model, const QString& userId, const QByteArray& roleName) {
    assert(model != nullptr);
    const int userIdRole = roleForName(model, QByteArrayLiteral("userId"));
    const int targetRole = roleForName(model, roleName);
    assert(userIdRole >= 0);
    assert(targetRole >= 0);

    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (model->data(index, userIdRole).toString() == userId) {
            return model->data(index, targetRole);
        }
    }
    return {};
}

quint32 routeRewriteSsrc(const QString& meetingId,
                        const QString& subscriberUserId,
                        quint32 sourceSsrc,
                        bool video) {
    if (sourceSsrc == 0U) {
        return 0U;
    }

    const QByteArray key = QStringLiteral("%1|%2|%3|%4")
                               .arg(meetingId, subscriberUserId, video ? QStringLiteral("video") : QStringLiteral("audio"))
                               .arg(sourceSsrc)
                               .toUtf8();
    quint32 hash = 2166136261U;
    for (const char ch : key) {
        hash ^= static_cast<quint32>(static_cast<unsigned char>(ch));
        hash *= 16777619U;
    }
    hash &= 0x7FFFFFFFU;
    if (hash == 0U || hash == sourceSsrc) {
        hash ^= 0x5A5A5A5AU;
    }
    return hash == 0U ? 0x13572468U : hash;
}

quint32 expectedMappedVideoSsrc(const QString& meetingId,
                                const QString& subscriberUserId,
                                quint32 assignedVideoSsrc,
                                quint32 sourceVideoSsrc,
                                bool multiPublisher) {
    if (!multiPublisher) {
        return assignedVideoSsrc != 0U ? assignedVideoSsrc : sourceVideoSsrc;
    }

    quint32 mapped = routeRewriteSsrc(meetingId, subscriberUserId, sourceVideoSsrc, true);
    if (mapped == assignedVideoSsrc || mapped == sourceVideoSsrc) {
        mapped ^= 0x01010101U;
    }
    if (mapped == 0U) {
        mapped = sourceVideoSsrc ^ 0x01010101U;
    }
    return mapped;
}

av::codec::DecodedVideoFrame makeTestFrame() {
    av::codec::DecodedVideoFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.pts = 1;
    frame.yPlane = {0, 16, 32, 48};
    frame.uvPlane = {64, 96};
    return frame;
}
bool waitForCondition(QCoreApplication& app, const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (condition()) {
            return true;
        }
        app.processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(20);
    }
    return condition();
}

}  // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    qputenv("MEETING_SYNTHETIC_SCREEN", QByteArrayLiteral("1"));

    QTemporaryDir tempDir;
    assert(tempDir.isValid());

    MeetingController controller(tempDir.filePath(QStringLiteral("settings.sqlite")));
    QStringList infoMessages;
    QObject::connect(&controller, &MeetingController::infoMessage, &app, [&](const QString& message) {
        infoMessages.append(message);
    });
    auto* signalingClient = controller.findChild<signaling::SignalingClient*>();
    assert(signalingClient != nullptr);
    auto* remoteVideoFrameStore = qobject_cast<av::render::VideoFrameStore*>(controller.remoteVideoFrameSource());
    assert(remoteVideoFrameStore != nullptr);
    auto activeVideoPeerUserId = [&controller]() {
        return controller.activeVideoPeerUserId();
    };
    assert(QMetaObject::invokeMethod(signalingClient,
                                     "loginFinished",
                                     Qt::DirectConnection,
                                     Q_ARG(bool, true),
                                     Q_ARG(QString, QStringLiteral("u1001")),
                                     Q_ARG(QString, QStringLiteral("resume-token")),
                                     Q_ARG(QString, QString())));
    assert(controller.loggedIn());

    meeting::MeetStateSyncNotify stateSync;
    stateSync.set_meeting_id("m-mute");
    stateSync.set_title("mute-room");
    stateSync.set_host_id("u1001");
    auto* participant = stateSync.add_participants();
    participant->set_user_id("u1001");
    participant->set_display_name("demo");
    participant->set_role(1);
    participant->set_is_audio_on(true);
    participant->set_is_video_on(true);
    participant->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.inMeeting());
    assert(!controller.audioMuted());
    assert(!controller.screenSharing());
    assert(controller.activeAudioPeerUserId().isEmpty());
    assert(!controller.hasActiveShare());

    assert(QMetaObject::invokeMethod(signalingClient,
                                     "mediaTransportAnswerReceived",
                                     Qt::DirectConnection,
                                     Q_ARG(QString, QStringLiteral("m-mute")),
                                     Q_ARG(QString, QStringLiteral("serverUfrag")),
                                     Q_ARG(QString, QStringLiteral("serverPwd")),
                                     Q_ARG(QString, QStringLiteral("AA:BB")),
                                     Q_ARG(QStringList, QStringList{QStringLiteral("candidate:1 1 udp 2130706431 127.0.0.1 5004 typ host")}),
                                     Q_ARG(quint32, 7777U),
                                     Q_ARG(quint32, 8888U)));

    infoMessages.clear();
    meeting::AuthKickNotify routeSwitching;
    routeSwitching.set_reason(R"({"stage":"switching","message":"Switching SFU media route"})");
    assert(emitProtoMessage(signalingClient, kMediaRouteStatusNotify, routeSwitching));
    assert(controller.statusText() == QStringLiteral("Switching SFU media route"));
    assert(infoMessages.count(QStringLiteral("Switching SFU media route")) == 0);

    meeting::AuthKickNotify routeSwitched;
    routeSwitched.set_reason(R"({"stage":"switched","message":"SFU media route switched to 10.0.0.3:10000","route":"10.0.0.3:10000"})");
    assert(emitProtoMessage(signalingClient, kMediaRouteStatusNotify, routeSwitched));
    assert(controller.statusText() == QStringLiteral("SFU media route switched to 10.0.0.3:10000"));
    assert(infoMessages.count(QStringLiteral("SFU media route switched to 10.0.0.3:10000")) == 1);

    assert(emitProtoMessage(signalingClient, kMediaRouteStatusNotify, routeSwitched));
    assert(infoMessages.count(QStringLiteral("SFU media route switched to 10.0.0.3:10000")) == 1);

    meeting::AuthKickNotify routeFailed;
    routeFailed.set_reason(R"({"stage":"failed","message":"SFU media route recovery failed"})");
    assert(emitProtoMessage(signalingClient, kMediaRouteStatusNotify, routeFailed));
    assert(controller.statusText() == QStringLiteral("SFU media route recovery failed"));
    assert(infoMessages.count(QStringLiteral("SFU media route recovery failed")) == 1);
    infoMessages.clear();

    controller.toggleScreenSharing();
    assert(controller.screenSharing());

    meeting::MeetMuteAllReq muteAll;
    muteAll.set_mute(true);
    assert(emitProtoMessage(signalingClient, kMeetMuteAll, muteAll));
    assert(controller.audioMuted());

    muteAll.set_mute(false);
    assert(emitProtoMessage(signalingClient, kMeetMuteAll, muteAll));
    assert(!controller.audioMuted());

    auto* remoteSharerA = stateSync.add_participants();
    remoteSharerA->set_user_id("u1002");
    remoteSharerA->set_display_name("sharer-a");
    remoteSharerA->set_role(0);
    remoteSharerA->set_is_audio_on(true);
    remoteSharerA->set_is_video_on(true);
    remoteSharerA->set_is_sharing(true);
    remoteSharerA->set_audio_ssrc(1111);
    remoteSharerA->set_video_ssrc(2222);

    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(participantData(controller.participantModel(), QStringLiteral("u1002"), QByteArrayLiteral("audioSsrc")).toUInt() == 1111U);
    assert(participantData(controller.participantModel(), QStringLiteral("u1002"), QByteArrayLiteral("videoSsrc")).toUInt() == 2222U);
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) == 8888U);
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(controller.hasActiveShare());
    assert(controller.activeShareUserId() == QStringLiteral("u1002"));
    assert(controller.activeShareDisplayName() == QStringLiteral("sharer-a"));

    auto* remoteSharerB = stateSync.add_participants();
    remoteSharerB->set_user_id("u1003");
    remoteSharerB->set_display_name("sharer-b");
    remoteSharerB->set_role(0);
    remoteSharerB->set_is_audio_on(true);
    remoteSharerB->set_is_video_on(true);
    remoteSharerB->set_is_sharing(true);
    remoteSharerB->set_audio_ssrc(3333);
    remoteSharerB->set_video_ssrc(4444);

    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(participantData(controller.participantModel(), QStringLiteral("u1002"), QByteArrayLiteral("audioSsrc")).toUInt() == 1111U);
    assert(participantData(controller.participantModel(), QStringLiteral("u1002"), QByteArrayLiteral("videoSsrc")).toUInt() == 2222U);
    assert(participantData(controller.participantModel(), QStringLiteral("u1003"), QByteArrayLiteral("audioSsrc")).toUInt() == 3333U);
    assert(participantData(controller.participantModel(), QStringLiteral("u1003"), QByteArrayLiteral("videoSsrc")).toUInt() == 4444U);
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 2222U, true));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 4444U, true));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) != controller.remoteVideoSsrcForUser(QStringLiteral("u1003")));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(controller.hasActiveShare());
    assert(controller.activeShareUserId() == QStringLiteral("u1002"));
    assert(controller.activeShareDisplayName() == QStringLiteral("sharer-a"));

    assert(controller.setActiveShareUserId(QStringLiteral("u1003")));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(controller.activeShareUserId() == QStringLiteral("u1003"));
    assert(controller.activeShareDisplayName() == QStringLiteral("sharer-b"));

    stateSync.mutable_participants(2)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 2222U, true));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 4444U, true));
    assert(controller.hasActiveShare());
    assert(controller.activeShareUserId() == QStringLiteral("u1002"));

    stateSync.mutable_participants(1)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 2222U, true));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 4444U, true));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(!controller.hasActiveShare());
    assert(controller.activeShareUserId().isEmpty());
    assert(controller.activeShareDisplayName().isEmpty());
    assert(activeVideoPeerUserId() == controller.activeAudioPeerUserId());
    assert(!controller.setActiveShareUserId(QStringLiteral("u1003")));
    av::codec::DecodedVideoFrame staleVideoFrame;
    remoteVideoFrameStore->setFrame(makeTestFrame());
    assert(remoteVideoFrameStore->snapshot(staleVideoFrame));
    assert(controller.setActiveVideoPeerUserId(QStringLiteral("u1003")));
    assert(activeVideoPeerUserId() == QStringLiteral("u1003"));
    assert(!remoteVideoFrameStore->snapshot(staleVideoFrame));
    assert(!controller.hasActiveShare());

    remoteVideoFrameStore->setFrame(makeTestFrame());
    assert(remoteVideoFrameStore->snapshot(staleVideoFrame));
    stateSync.mutable_participants(2)->set_is_video_on(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) == 8888U);
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) == 0U);
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(activeVideoPeerUserId() == QStringLiteral("u1002"));
    assert(!controller.setActiveVideoPeerUserId(QStringLiteral("u1003")));
    assert(!remoteVideoFrameStore->snapshot(staleVideoFrame));

    stateSync.mutable_participants(2)->set_is_video_on(true);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 2222U, true));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) ==
           expectedMappedVideoSsrc(QStringLiteral("m-mute"), QStringLiteral("u1001"), 8888U, 4444U, true));
    assert(controller.setActiveVideoPeerUserId(QStringLiteral("u1003")));
    assert(activeVideoPeerUserId() == QStringLiteral("u1003"));

    stateSync.mutable_participants()->DeleteSubrange(1, 1);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1002")) == 0U);
    assert(controller.remoteVideoSsrcForUser(QStringLiteral("u1003")) == 8888U);
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1003"));

    stateSync.mutable_participants(0)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(!controller.screenSharing());

    return 0;
}


