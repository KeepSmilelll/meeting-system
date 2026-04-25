#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "server/DtlsTransport.h"
#include "server/IceLiteSession.h"
#include "server/SrtpSession.h"

namespace sfu {

class ParticipantTransport final {
public:
    ParticipantTransport() = default;
    ParticipantTransport(ParticipantTransport&&) noexcept = default;
    ParticipantTransport& operator=(ParticipantTransport&&) noexcept = default;
    ParticipantTransport(const ParticipantTransport&) = delete;
    ParticipantTransport& operator=(const ParticipantTransport&) = delete;

    ParticipantTransport(std::string meetingId,
                         std::string userId,
                         bool publishAudio,
                         bool publishVideo,
                         std::string clientDtlsFingerprint,
                         uint32_t assignedAudioSsrc,
                         uint32_t assignedVideoSsrc,
                         IceLiteSession iceSession,
                         std::unique_ptr<DtlsTransport> dtlsTransport)
        : meetingId_(std::move(meetingId))
        , userId_(std::move(userId))
        , publishAudio_(publishAudio)
        , publishVideo_(publishVideo)
        , clientDtlsFingerprint_(std::move(clientDtlsFingerprint))
        , assignedAudioSsrc_(assignedAudioSsrc)
        , assignedVideoSsrc_(assignedVideoSsrc)
        , iceSession_(std::move(iceSession))
        , dtlsTransport_(std::move(dtlsTransport)) {}

    const std::string& MeetingId() const noexcept { return meetingId_; }
    const std::string& UserId() const noexcept { return userId_; }
    bool PublishAudio() const noexcept { return publishAudio_; }
    bool PublishVideo() const noexcept { return publishVideo_; }
    const std::string& ClientDtlsFingerprint() const noexcept { return clientDtlsFingerprint_; }
    uint32_t AssignedAudioSsrc() const noexcept { return assignedAudioSsrc_; }
    uint32_t AssignedVideoSsrc() const noexcept { return assignedVideoSsrc_; }

    bool OwnsSsrc(uint32_t ssrc) const noexcept {
        return ssrc != 0U && (assignedAudioSsrc_ == ssrc || assignedVideoSsrc_ == ssrc);
    }

    IceLiteSession& IceSession() noexcept { return iceSession_; }
    const IceLiteSession& IceSession() const noexcept { return iceSession_; }
    bool HasDtlsTransport() const noexcept { return dtlsTransport_ != nullptr; }
    bool DtlsConnected() const noexcept { return dtlsTransport_ != nullptr && dtlsTransport_->IsConnected(); }
    bool SrtpReady() const noexcept { return srtpReady_; }
    const std::string& LastError() const noexcept { return lastError_; }

    bool HandleDtlsDatagram(const uint8_t* data,
                            std::size_t len,
                            std::vector<std::vector<uint8_t>>* outgoingPackets) {
        if (dtlsTransport_ == nullptr) {
            lastError_ = "DTLS transport is not configured";
            return false;
        }
        if (!dtlsTransport_->HandleIncomingDatagram(data, len, outgoingPackets)) {
            lastError_ = dtlsTransport_->LastError();
            return false;
        }
        if (dtlsTransport_->IsConnected() && !srtpReady_ && !ConfigureSrtpSessions()) {
            return false;
        }
        lastError_.clear();
        return true;
    }

    bool ProtectRtp(std::vector<uint8_t>* packet) {
        if (!srtpReady_) {
            lastError_ = "SRTP is not ready";
            return false;
        }
        if (!outboundSrtp_.ProtectRtp(packet)) {
            lastError_ = outboundSrtp_.LastError();
            return false;
        }
        lastError_.clear();
        return true;
    }

    bool UnprotectRtp(std::vector<uint8_t>* packet) {
        if (!srtpReady_) {
            lastError_ = "SRTP is not ready";
            return false;
        }
        if (!inboundSrtp_.UnprotectRtp(packet)) {
            lastError_ = inboundSrtp_.LastError();
            return false;
        }
        lastError_.clear();
        return true;
    }

    bool ProtectRtcp(std::vector<uint8_t>* packet) {
        if (!srtpReady_) {
            lastError_ = "SRTP is not ready";
            return false;
        }
        if (!outboundSrtp_.ProtectRtcp(packet)) {
            lastError_ = outboundSrtp_.LastError();
            return false;
        }
        lastError_.clear();
        return true;
    }

    bool UnprotectRtcp(std::vector<uint8_t>* packet) {
        if (!srtpReady_) {
            lastError_ = "SRTP is not ready";
            return false;
        }
        if (!inboundSrtp_.UnprotectRtcp(packet)) {
            lastError_ = inboundSrtp_.LastError();
            return false;
        }
        lastError_.clear();
        return true;
    }

private:
    bool ConfigureSrtpSessions() {
        if (dtlsTransport_ == nullptr || !dtlsTransport_->IsConnected()) {
            lastError_ = "DTLS transport is not connected";
            return false;
        }

        DtlsTransport::SrtpKeyMaterial keying{};
        constexpr std::size_t kMasterKeyLen = 16U;
        constexpr std::size_t kMasterSaltLen = 14U;
        if (!dtlsTransport_->ExportSrtpKeyMaterial(kMasterKeyLen, kMasterSaltLen, &keying)) {
            lastError_ = "DTLS SRTP exporter failed";
            return false;
        }
        if (!inboundSrtp_.Configure(keying.remoteKey, keying.remoteSalt, SrtpSession::Direction::Inbound) ||
            !outboundSrtp_.Configure(keying.localKey, keying.localSalt, SrtpSession::Direction::Outbound)) {
            lastError_ = inboundSrtp_.LastError().empty() ? outboundSrtp_.LastError() : inboundSrtp_.LastError();
            return false;
        }
        srtpReady_ = true;
        lastError_.clear();
        return true;
    }

    std::string meetingId_;
    std::string userId_;
    bool publishAudio_{false};
    bool publishVideo_{false};
    std::string clientDtlsFingerprint_;
    uint32_t assignedAudioSsrc_{0};
    uint32_t assignedVideoSsrc_{0};
    IceLiteSession iceSession_;
    std::unique_ptr<DtlsTransport> dtlsTransport_;
    SrtpSession inboundSrtp_;
    SrtpSession outboundSrtp_;
    bool srtpReady_{false};
    std::string lastError_;
};

} // namespace sfu
