#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "server/ParticipantTransport.h"

namespace sfu {

class TransportRegistry final {
public:
    static std::string Kind(bool publishAudio, bool publishVideo) {
        if (publishAudio && publishVideo) {
            return "av";
        }
        if (publishVideo) {
            return "video";
        }
        return "audio";
    }

    static std::string Key(const std::string& meetingId,
                           const std::string& userId,
                           const std::string& mediaKind) {
        return meetingId + "|" + userId + "|" + mediaKind;
    }

    void Upsert(ParticipantTransport transport, const std::string& mediaKind) {
        std::lock_guard<std::mutex> lock(mutex_);
        transports_[Key(transport.MeetingId(), transport.UserId(), mediaKind)] = std::move(transport);
    }

    bool AppendIceCandidate(const std::string& meetingId,
                            const std::string& userId,
                            const std::string& candidate,
                            bool endOfCandidates) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool updated = false;
        for (auto& [key, transport] : transports_) {
            (void)key;
            if (transport.MeetingId() != meetingId || transport.UserId() != userId) {
                continue;
            }
            transport.IceSession().AppendClientCandidate(candidate);
            if (endOfCandidates) {
                transport.IceSession().MarkEndOfCandidates();
            }
            updated = true;
        }
        return updated;
    }

    bool EraseUserTransports(const std::string& meetingId, const std::string& userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool erased = false;
        for (auto it = transports_.begin(); it != transports_.end();) {
            if (it->second.MeetingId() == meetingId && it->second.UserId() == userId) {
                it = transports_.erase(it);
                erased = true;
            } else {
                ++it;
            }
        }
        return erased;
    }

    bool SelectEndpointForUsername(const std::string& username,
                                   const UdpServer::Endpoint& endpoint,
                                   std::string* meetingId,
                                   std::string* userId,
                                   std::string* mediaKind = nullptr) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [key, transport] : transports_) {
            if (!transport.IceSession().MatchesUsername(username)) {
                continue;
            }
            transport.IceSession().SelectEndpoint(endpoint);
            if (meetingId != nullptr) {
                *meetingId = transport.MeetingId();
            }
            if (userId != nullptr) {
                *userId = transport.UserId();
            }
            if (mediaKind != nullptr) {
                const auto delimiter = key.rfind('|');
                *mediaKind = delimiter == std::string::npos ? std::string{} : key.substr(delimiter + 1U);
            }
            return true;
        }
        return false;
    }

    bool TransportAllowsSsrcFrom(uint32_t ssrc, const UdpServer::Endpoint& endpoint) const {
        if (ssrc == 0U) {
            return true;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        bool foundStructuredTransport = false;
        for (const auto& [key, transport] : transports_) {
            (void)key;
            if (!transport.OwnsSsrc(ssrc)) {
                continue;
            }
            foundStructuredTransport = true;
            if (transport.IceSession().IceConnected() &&
                transport.IceSession().HasSelectedEndpoint() &&
                SameEndpoint(transport.IceSession().SelectedEndpoint(), endpoint)) {
                return true;
            }
        }
        return !foundStructuredTransport;
    }

    bool HasTransportForSsrc(uint32_t ssrc) const {
        if (ssrc == 0U) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, transport] : transports_) {
            (void)key;
            if (transport.OwnsSsrc(ssrc)) {
                return true;
            }
        }
        return false;
    }

    bool HasSelectedTransportForEndpoint(const UdpServer::Endpoint& endpoint) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, transport] : transports_) {
            (void)key;
            if (transport.IceSession().HasSelectedEndpoint() &&
                SameEndpoint(transport.IceSession().SelectedEndpoint(), endpoint)) {
                return true;
            }
        }
        return false;
    }

    bool HandleDtlsFromEndpoint(const UdpServer::Endpoint& endpoint,
                                const uint8_t* data,
                                std::size_t len,
                                std::vector<std::vector<uint8_t>>* outgoingPackets) {
        std::lock_guard<std::mutex> lock(mutex_);
        ParticipantTransport* transport = FindBySelectedEndpoint(endpoint);
        return transport != nullptr &&
               transport->IceSession().IceConnected() &&
               transport->HandleDtlsDatagram(data, len, outgoingPackets);
    }

    bool SrtpReadyForSsrcFrom(uint32_t ssrc, const UdpServer::Endpoint& endpoint) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, transport] : transports_) {
            (void)key;
            if (transport.OwnsSsrc(ssrc) &&
                transport.IceSession().HasSelectedEndpoint() &&
                SameEndpoint(transport.IceSession().SelectedEndpoint(), endpoint)) {
                return transport.SrtpReady();
            }
        }
        return false;
    }

    bool UnprotectRtpFromEndpoint(const UdpServer::Endpoint& endpoint, std::vector<uint8_t>* packet) {
        if (packet == nullptr || packet->size() < 12U) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ParticipantTransport* transport = FindBySelectedEndpoint(endpoint);
        return transport != nullptr && transport->UnprotectRtp(packet);
    }

    bool UnprotectRtcpFromEndpoint(const UdpServer::Endpoint& endpoint, std::vector<uint8_t>* packet) {
        if (packet == nullptr || packet->empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ParticipantTransport* transport = FindBySelectedEndpoint(endpoint);
        return transport != nullptr && transport->UnprotectRtcp(packet);
    }

    bool ProtectRtpToEndpoint(const UdpServer::Endpoint& endpoint, std::vector<uint8_t>* packet) {
        if (packet == nullptr || packet->empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ParticipantTransport* transport = FindBySelectedEndpoint(endpoint);
        return transport != nullptr && transport->ProtectRtp(packet);
    }

    bool ProtectRtcpToEndpoint(const UdpServer::Endpoint& endpoint, std::vector<uint8_t>* packet) {
        if (packet == nullptr || packet->empty()) {
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ParticipantTransport* transport = FindBySelectedEndpoint(endpoint);
        return transport != nullptr && transport->ProtectRtcp(packet);
    }

private:
    ParticipantTransport* FindBySelectedEndpoint(const UdpServer::Endpoint& endpoint) {
        for (auto& [key, transport] : transports_) {
            (void)key;
            if (transport.IceSession().HasSelectedEndpoint() &&
                SameEndpoint(transport.IceSession().SelectedEndpoint(), endpoint)) {
                return &transport;
            }
        }
        return nullptr;
    }

    static bool SameEndpoint(const UdpServer::Endpoint& left, const UdpServer::Endpoint& right) {
        return left.sin_family == AF_INET &&
               right.sin_family == AF_INET &&
               left.sin_addr.s_addr == right.sin_addr.s_addr &&
               left.sin_port == right.sin_port;
    }

    mutable std::mutex mutex_;
    std::unordered_map<std::string, ParticipantTransport> transports_;
};

} // namespace sfu
