#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "server/RpcProtocol.h"

#include "proto/pb/sfu_rpc.pb.h"
#include "room/RoomManager.h"

namespace sfu {

class MediaIngress;

class RpcService final {
public:
    explicit RpcService(std::shared_ptr<RoomManager> roomManager = std::make_shared<RoomManager>(),
                        std::shared_ptr<MediaIngress> mediaIngress = nullptr);

    RoomManager* roomManager() const noexcept;
    MediaIngress* mediaIngress() const noexcept;
    uint16_t MediaPort() const noexcept;

    void SetAdvertisedAddress(std::string address);
    const std::string& AdvertisedAddress() const noexcept;

    bool HandleCreateRoom(const sfu_rpc::CreateRoomReq& req, sfu_rpc::CreateRoomRsp* rsp) const;
    bool HandleDestroyRoom(const sfu_rpc::DestroyRoomReq& req, sfu_rpc::DestroyRoomRsp* rsp) const;
    bool HandleSetupTransport(const sfu_rpc::SetupTransportReq& req, sfu_rpc::SetupTransportRsp* rsp) const;
    bool HandleTrickleIceCandidate(const sfu_rpc::TrickleIceCandidateReq& req, sfu_rpc::TrickleIceCandidateRsp* rsp) const;
    bool HandleCloseTransport(const sfu_rpc::CloseTransportReq& req, sfu_rpc::CloseTransportRsp* rsp) const;
    bool HandleAddPublisher(const sfu_rpc::AddPublisherReq& req, sfu_rpc::AddPublisherRsp* rsp) const;
    bool HandleAddSubscriber(const sfu_rpc::AddSubscriberReq& req, sfu_rpc::AddSubscriberRsp* rsp) const;
    bool HandleRemovePublisher(const sfu_rpc::RemovePublisherReq& req, sfu_rpc::RemovePublisherRsp* rsp) const;
    bool HandleRemoveSubscriber(const sfu_rpc::RemoveSubscriberReq& req, sfu_rpc::RemoveSubscriberRsp* rsp) const;
    bool HandleGetNodeStatus(const sfu_rpc::GetNodeStatusReq& req, sfu_rpc::GetNodeStatusRsp* rsp) const;

    bool Dispatch(RpcMethod method, const uint8_t* payload, std::size_t len, std::vector<uint8_t>* responsePayload) const;

private:
    template <typename MessageT>
    static bool SerializeMessage(const MessageT& message, std::vector<uint8_t>* out);

    std::shared_ptr<RoomManager> roomManager_;
    std::shared_ptr<MediaIngress> mediaIngress_;
    mutable std::string advertisedAddress_;
};

} // namespace sfu
