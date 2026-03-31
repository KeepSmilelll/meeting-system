#include "av/codec/AudioDecoder.h"
#include "av/render/AudioPlayer.h"
#include "av/session/AudioCallSession.h"
#include "net/media/JitterBuffer.h"
#include "net/media/RTCPHandler.h"
#include "net/media/RTPReceiver.h"

#include <iostream>
#include <string>

int main() {
    std::string audioError;
    const bool audioOk = av::codec::runAudioPipelineLoopbackSelfCheck(&audioError);
    const bool playerOk = av::render::runAudioPlayerSelfCheck();
    std::string callError;
    const bool callOk = av::session::runAudioCallSessionLoopbackSelfCheck(&callError);
    const bool rtpOk = media::runRtpLoopbackSelfCheck();
    const bool jitterOk = media::runJitterBufferSelfCheck();
    const bool rtcpOk = media::runRtcpMainFlowSelfCheck();

    const bool pipelineOk = audioOk || callOk;
    if (pipelineOk && playerOk && callOk && rtpOk && jitterOk && rtcpOk) {
        return 0;
    }

    std::cerr << "audio_codec_smoke failed: "
              << "audio=" << audioOk << " "
              << "player=" << playerOk << " "
              << "call=" << callOk << " "
              << "rtp=" << rtpOk << " "
              << "jitter=" << jitterOk << " "
              << "rtcp=" << rtcpOk;
    if (!audioOk && !audioError.empty()) {
        std::cerr << " audio_error=" << audioError;
    }
    if (!callOk && !callError.empty()) {
        std::cerr << " call_error=" << callError;
    }
    std::cerr << std::endl;
    return 1;
}
