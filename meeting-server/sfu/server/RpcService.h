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
    bool HandleAddPublisher(const sfu_rpc::AddPublisherReq& req, sfu_rpc::AddPublisherRsp* rsp) const;
    bool HandleRemovePublisher(const sfu_rpc::RemovePublisherReq& req, sfu_rpc::RemovePublisherRsp* rsp) const;

    bool Dispatch(RpcMethod method, const uint8_t* payload, std::size_t len, std::vector<uint8_t>* responsePayload) const;

private:
    template <typename MessageT>
    static bool SerializeMessage(const MessageT& message, std::vector<uint8_t>* out);

    std::shared_ptr<RoomManager> roomManager_;
    std::shared_ptr<MediaIngress> mediaIngress_;
    mutable std::string advertisedAddress_;
};

} // namespace sfu
