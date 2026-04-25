#pragma once

#include <string>
#include <vector>

#include "server/UdpServer.h"

namespace sfu {

class IceLiteSession final {
public:
    void Configure(std::string clientIceUfrag,
                   std::string clientIcePwd,
                   std::vector<std::string> clientCandidates,
                   std::string serverIceUfrag,
                   std::string serverIcePwd,
                   std::vector<std::string> serverCandidates) {
        clientIceUfrag_ = std::move(clientIceUfrag);
        clientIcePwd_ = std::move(clientIcePwd);
        clientCandidates_ = std::move(clientCandidates);
        serverIceUfrag_ = std::move(serverIceUfrag);
        serverIcePwd_ = std::move(serverIcePwd);
        serverCandidates_ = std::move(serverCandidates);
        endOfCandidates_ = false;
        iceConnected_ = false;
        hasSelectedEndpoint_ = false;
        selectedEndpoint_ = UdpServer::Endpoint{};
    }

    const std::string& ClientIceUfrag() const noexcept { return clientIceUfrag_; }
    const std::string& ClientIcePwd() const noexcept { return clientIcePwd_; }
    const std::vector<std::string>& ClientCandidates() const noexcept { return clientCandidates_; }
    const std::string& ServerIceUfrag() const noexcept { return serverIceUfrag_; }
    const std::string& ServerIcePwd() const noexcept { return serverIcePwd_; }
    const std::vector<std::string>& ServerCandidates() const noexcept { return serverCandidates_; }

    void AppendClientCandidate(std::string candidate) {
        if (!candidate.empty()) {
            clientCandidates_.push_back(std::move(candidate));
        }
    }

    void MarkEndOfCandidates() noexcept { endOfCandidates_ = true; }
    bool EndOfCandidates() const noexcept { return endOfCandidates_; }

    bool MatchesUsername(const std::string& username) const {
        if (username.empty() || clientIceUfrag_.empty() || serverIceUfrag_.empty()) {
            return false;
        }
        const std::string expectedUsername = serverIceUfrag_ + ":" + clientIceUfrag_;
        const std::string toleratedReverseUsername = clientIceUfrag_ + ":" + serverIceUfrag_;
        return username == expectedUsername || username == toleratedReverseUsername;
    }

    void SelectEndpoint(const UdpServer::Endpoint& endpoint) {
        selectedEndpoint_ = endpoint;
        hasSelectedEndpoint_ = true;
        iceConnected_ = true;
    }

    bool IceConnected() const noexcept { return iceConnected_; }
    bool HasSelectedEndpoint() const noexcept { return hasSelectedEndpoint_; }
    const UdpServer::Endpoint& SelectedEndpoint() const noexcept { return selectedEndpoint_; }

private:
    std::string clientIceUfrag_;
    std::string clientIcePwd_;
    std::vector<std::string> clientCandidates_;
    std::string serverIceUfrag_;
    std::string serverIcePwd_;
    std::vector<std::string> serverCandidates_;
    bool endOfCandidates_{false};
    bool iceConnected_{false};
    bool hasSelectedEndpoint_{false};
    UdpServer::Endpoint selectedEndpoint_{};
};

} // namespace sfu
