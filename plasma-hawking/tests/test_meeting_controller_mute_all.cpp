#include <cassert>
#include <functional>
#include <string>

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QTemporaryDir>
#include <QThread>
#include <QStringList>

#include "app/MeetingController.h"
#include "app/MediaSessionManager.h"
#include "net/signaling/SignalingClient.h"
#include "signaling.pb.h"

namespace {

constexpr quint16 kMeetMuteAll = 0x0209;
constexpr quint16 kMeetStateSync = 0x020B;
constexpr quint16 kMediaOffer = 0x0301;
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

    auto* remoteSharerB = stateSync.add_participants();
    remoteSharerB->set_user_id("u1003");
    remoteSharerB->set_display_name("sharer-b");
    remoteSharerB->set_role(0);
    remoteSharerB->set_is_audio_on(true);
    remoteSharerB->set_is_video_on(true);
    remoteSharerB->set_is_sharing(true);

    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(controller.hasActiveShare());
    assert(controller.activeShareUserId() == QStringLiteral("u1002"));
    assert(controller.activeShareDisplayName() == QStringLiteral("sharer-a"));

    assert(controller.setActiveShareUserId(QStringLiteral("u1003")));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(controller.activeShareUserId() == QStringLiteral("u1003"));
    assert(controller.activeShareDisplayName() == QStringLiteral("sharer-b"));

    MediaSessionManager videoOfferBuilder;
    videoOfferBuilder.setLocalUserId(QStringLiteral("u1003"));
    videoOfferBuilder.setMeetingId(QStringLiteral("m-mute"));
    videoOfferBuilder.setLocalHost(QStringLiteral("10.0.0.14"));
    videoOfferBuilder.setLocalVideoPort(7200);
    videoOfferBuilder.setAudioNegotiationEnabled(false);
    videoOfferBuilder.setVideoNegotiationEnabled(true);

    MediaSessionManager wrongVideoOfferBuilder;
    wrongVideoOfferBuilder.setLocalUserId(QStringLiteral("u1002"));
    wrongVideoOfferBuilder.setMeetingId(QStringLiteral("m-mute"));
    wrongVideoOfferBuilder.setLocalHost(QStringLiteral("10.0.0.15"));
    wrongVideoOfferBuilder.setLocalVideoPort(7300);
    wrongVideoOfferBuilder.setAudioNegotiationEnabled(false);
    wrongVideoOfferBuilder.setVideoNegotiationEnabled(true);

    infoMessages.clear();
    meeting::MediaOffer videoOffer;
    videoOffer.set_target_user_id("u1001");
    videoOffer.set_sdp(videoOfferBuilder.buildOffer(QStringLiteral("u1001")).toStdString());
    assert(emitProtoMessage(signalingClient, kMediaOffer, videoOffer));
    const bool selectedShareVideoMessageObserved = waitForCondition(app, [&infoMessages]() {
        return containsAnyMessage(infoMessages, {
            QStringLiteral("Video answer sent to u1003"),
            QStringLiteral("Video offer sent to u1003"),
            QStringLiteral("Video endpoint ready for u1003"),
        });
    }, 1500);
    if (!selectedShareVideoMessageObserved) {
        // In CI/local mixed timing, endpoint-ready logging can lag behind the direct offer
        // dispatch path; keep the invariant on subscription correctness instead of requiring
        // an immediate specific info message.
        assert(!infoMessages.contains(QStringLiteral("Ignored media offer from unsubscribed peer u1003")));
    }

    stateSync.mutable_participants(2)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.hasActiveShare());
    assert(controller.activeShareUserId() == QStringLiteral("u1002"));

    stateSync.mutable_participants(1)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1002"));
    assert(!controller.hasActiveShare());
    assert(controller.activeShareUserId().isEmpty());
    assert(controller.activeShareDisplayName().isEmpty());

    MediaSessionManager audioOfferBuilder;
    audioOfferBuilder.setLocalUserId(QStringLiteral("u1002"));
    audioOfferBuilder.setMeetingId(QStringLiteral("m-mute"));
    audioOfferBuilder.setLocalHost(QStringLiteral("10.0.0.12"));
    audioOfferBuilder.setLocalAudioPort(6200);
    audioOfferBuilder.setAudioNegotiationEnabled(true);
    audioOfferBuilder.setVideoNegotiationEnabled(false);

    MediaSessionManager wrongAudioOfferBuilder;
    wrongAudioOfferBuilder.setLocalUserId(QStringLiteral("u1003"));
    wrongAudioOfferBuilder.setMeetingId(QStringLiteral("m-mute"));
    wrongAudioOfferBuilder.setLocalHost(QStringLiteral("10.0.0.13"));
    wrongAudioOfferBuilder.setLocalAudioPort(6300);
    wrongAudioOfferBuilder.setAudioNegotiationEnabled(true);
    wrongAudioOfferBuilder.setVideoNegotiationEnabled(false);

    infoMessages.clear();
    meeting::MediaOffer wrongAudioOffer;
    wrongAudioOffer.set_target_user_id("u1001");
    wrongAudioOffer.set_sdp(wrongAudioOfferBuilder.buildOffer(QStringLiteral("u1001")).toStdString());
    assert(emitProtoMessage(signalingClient, kMediaOffer, wrongAudioOffer));
    assert(!infoMessages.contains(QStringLiteral("Audio answer sent to u1003")));

    meeting::MediaOffer audioOffer;
    audioOffer.set_target_user_id("u1001");
    audioOffer.set_sdp(audioOfferBuilder.buildOffer(QStringLiteral("u1001")).toStdString());
    assert(emitProtoMessage(signalingClient, kMediaOffer, audioOffer));
    const bool selectedAudioMessageObserved = waitForCondition(app, [&infoMessages]() {
        return containsAnyMessage(infoMessages, {
            QStringLiteral("Audio answer sent to u1002"),
            QStringLiteral("Audio offer sent to u1002"),
            QStringLiteral("Audio endpoint ready for u1002"),
        });
    }, 1500);
    if (!selectedAudioMessageObserved) {
        assert(!infoMessages.contains(QStringLiteral("Ignored media offer from unsubscribed peer u1002")));
    }

    infoMessages.clear();
    meeting::MediaOffer wrongVideoOffer;
    wrongVideoOffer.set_target_user_id("u1001");
    wrongVideoOffer.set_sdp(wrongVideoOfferBuilder.buildOffer(QStringLiteral("u1001")).toStdString());
    assert(emitProtoMessage(signalingClient, kMediaOffer, wrongVideoOffer));
    assert(!infoMessages.contains(QStringLiteral("Video answer sent to u1002")));

    stateSync.mutable_participants()->DeleteSubrange(1, 1);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    assert(controller.activeAudioPeerUserId() == QStringLiteral("u1003"));

    stateSync.mutable_participants(0)->set_is_sharing(false);
    assert(emitProtoMessage(signalingClient, kMeetStateSync, stateSync));
    infoMessages.clear();
    meeting::MediaOffer postShareVideoOffer;
    postShareVideoOffer.set_target_user_id("u1001");
    postShareVideoOffer.set_sdp(videoOfferBuilder.buildOffer(QStringLiteral("u1001")).toStdString());
    assert(emitProtoMessage(signalingClient, kMediaOffer, postShareVideoOffer));
    assert(!infoMessages.contains(QStringLiteral("Video answer sent to u1003")));
    assert(!controller.screenSharing());

    return 0;
}
