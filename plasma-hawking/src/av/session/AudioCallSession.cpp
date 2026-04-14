#include "AudioCallSession.h"

bool av::session::runAudioCallSessionLoopbackSelfCheck(std::string* error) {
#ifdef _WIN32
    AudioCallSessionConfig config{};
    config.peerAddress = "127.0.0.1";

    AudioCallSession session(config);
    if (!session.start()) {
        if (error != nullptr) {
            *error = session.lastError();
        }
        session.stop();
        return false;
    }

    const uint16_t localPort = session.localPort();
    if (localPort == 0) {
        if (error != nullptr) {
            *error = "local port not assigned";
        }
        session.stop();
        return false;
    }

    session.setPeer("127.0.0.1", localPort);

    const int frameSamples = session.player().frameSamples();

    av::capture::AudioFrame frame;
    frame.sampleRate = 48000;
    frame.channels = 1;
    frame.samples.assign(static_cast<std::size_t>(frameSamples), 0.2F);

    for (int i = 0; i < 20; ++i) {
        av::capture::AudioFrame submitFrame = frame;
        submitFrame.pts = static_cast<int64_t>(frameSamples * i);
        if (!session.submitLocalFrame(submitFrame)) {
            if (error != nullptr) {
                *error = "submit local frame failed";
            }
            session.stop();
            return false;
        }
    }

    av::capture::AudioFrame played;
    const bool gotPlayed = session.waitForPlayedFrame(played, std::chrono::milliseconds(5000));
    uint32_t observedRttMs = 0;
    for (int i = 0; i < 40 && observedRttMs == 0; ++i) {
        observedRttMs = session.lastRttMs();
        if (observedRttMs != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const uint32_t observedTargetBitrate = session.targetBitrateBps();
    const bool ok = gotPlayed &&
                    played.sampleRate == 48000 &&
                    played.channels == 1 &&
                    played.samples.size() == static_cast<std::size_t>(frameSamples) &&
                    observedRttMs > 0 &&
                    observedTargetBitrate >= 16000U &&
                    observedTargetBitrate <= 64000U;

    if (!ok && error != nullptr) {
        if (!session.lastError().empty()) {
            *error = session.lastError();
        } else if (!gotPlayed) {
            *error = "playback timeout";
        } else if (observedRttMs == 0) {
            *error = "rtcp rtt timeout";
        } else if (observedTargetBitrate < 16000U || observedTargetBitrate > 64000U) {
            *error = "audio bwe bitrate out of range";
        } else {
            *error = "unexpected decoded frame shape";
        }
    }

    session.stop();
    return ok;
#else
    return false;
#endif
}
