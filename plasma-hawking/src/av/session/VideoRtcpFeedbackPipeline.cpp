#include "VideoRtcpFeedbackPipeline.h"

#include <algorithm>

namespace av::session {

bool VideoRtcpFeedbackPipeline::parseFeedback(const uint8_t* data,
                                              std::size_t len,
                                              uint32_t localSsrc,
                                              std::vector<VideoRtcpFeedbackEvent>& outEvents) const {
    outEvents.clear();

    std::vector<media::RTCPPacketSlice> slices;
    if (!m_rtcpHandler.parseCompoundPacket(data, len, slices)) {
        return false;
    }

    for (const auto& slice : slices) {
        if (!m_rtcpHandler.isFeedbackPacket(slice.header)) {
            continue;
        }

        if (slice.header.packetType == 205U && slice.header.countOrFormat == 1U) {
            media::RTCPNackFeedback nack{};
            if (!m_rtcpHandler.parseNackFeedback(data + slice.offset, slice.size, nack)) {
                continue;
            }
            if (localSsrc == 0U || (nack.mediaSsrc != 0U && nack.mediaSsrc != localSsrc)) {
                continue;
            }

            outEvents.reserve(outEvents.size() + nack.lostSequences.size());
            for (const uint16_t sequenceNumber : nack.lostSequences) {
                VideoRtcpFeedbackEvent event{};
                event.kind = VideoRtcpFeedbackEventKind::Nack;
                event.sequenceNumber = sequenceNumber;
                event.mediaSsrc = nack.mediaSsrc;
                outEvents.push_back(event);
            }
            continue;
        }

        if (slice.header.packetType == 206U && slice.header.countOrFormat == 1U) {
            media::RTCPPliFeedback pli{};
            if (!m_rtcpHandler.parsePliFeedback(data + slice.offset, slice.size, pli)) {
                continue;
            }
            if (localSsrc == 0U || (pli.mediaSsrc != 0U && pli.mediaSsrc != localSsrc)) {
                continue;
            }

            VideoRtcpFeedbackEvent event{};
            event.kind = VideoRtcpFeedbackEventKind::Pli;
            event.mediaSsrc = pli.mediaSsrc;
            outEvents.push_back(event);
            continue;
        }

        if (slice.header.packetType == 206U && slice.header.countOrFormat == 15U) {
            media::RTCPRembFeedback remb{};
            if (!m_rtcpHandler.parseRembFeedback(data + slice.offset, slice.size, remb)) {
                continue;
            }
            const auto matched = std::find(remb.targetSsrcs.begin(), remb.targetSsrcs.end(), localSsrc);
            if (localSsrc == 0U || matched == remb.targetSsrcs.end()) {
                continue;
            }

            VideoRtcpFeedbackEvent event{};
            event.kind = VideoRtcpFeedbackEventKind::Remb;
            event.bitrateBps = remb.bitrateBps;
            event.mediaSsrc = remb.mediaSsrc;
            outEvents.push_back(event);
        }
    }

    return true;
}

}  // namespace av::session
