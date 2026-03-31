#include "server/RpcService.h"

#include <memory>
#include <string>
#include <utility>

#include "server/MediaIngress.h"

namespace sfu {
namespace {

constexpr std::size_t kDefaultMaxPublishers = 16;

} // namespace

RpcService::RpcService(std::shared_ptr<RoomManager> roomManager, std::shared_ptr<MediaIngress> mediaIngress)
    : roomManager_(roomManager ? std::move(roomManager) : std::make_shared<RoomManager>())
    , mediaIngress_(mediaIngress ? std::move(mediaIngress) : std::make_shared<MediaIngress>(roomManager_))
    , advertisedAddress_("127.0.0.1:0") {}

RoomManager* RpcService::roomManager() const noexcept {
    return roomManager_.get();
}

MediaIngress* RpcService::mediaIngress() const noexcept {
    return mediaIngress_.get();
}

uint16_t RpcService::MediaPort() const noexcept {
    return mediaIngress_ ? mediaIngress_->Port() : 0;
}

void RpcService::SetAdvertisedAddress(std::string address) {
    advertisedAddress_ = std::move(address);
}

const std::string& RpcService::AdvertisedAddress() const noexcept {
    return advertisedAddress_;
}

template <typename MessageT>
bool RpcService::SerializeMessage(const MessageT& message, std::vector<uint8_t>* out) {
    if (out == nullptr) {
        return false;
    }

    std::string bytes;
    if (!message.SerializeToString(&bytes)) {
        return false;
    }

    out->assign(bytes.begin(), bytes.end());
    return true;
}

bool RpcService::HandleCreateRoom(const sfu_rpc::CreateRoomReq& req, sfu_rpc::CreateRoomRsp* rsp) const {
    if (rsp == nullptr) {
        return false;
    }

    rsp->set_success(false);
    rsp->set_sfu_address(advertisedAddress_);

    if (req.meeting_id().empty()) {
        return true;
    }

    auto maxPublishers = req.max_publishers();
    if (maxPublishers <= 0) {
        maxPublishers = static_cast<int32_t>(kDefaultMaxPublishers);
    }

    rsp->set_success(roomManager_ && roomManager_->CreateRoom(req.meeting_id(), static_cast<std::size_t>(maxPublishers)));
    return true;
}

bool RpcService::HandleDestroyRoom(const sfu_rpc::DestroyRoomReq& req, sfu_rpc::DestroyRoomRsp* rsp) const {
    if (rsp == nullptr) {
        return false;
    }

    rsp->set_success(roomManager_ && roomManager_->DestroyRoom(req.meeting_id()));
    return true;
}

bool RpcService::HandleAddPublisher(const sfu_rpc::AddPublisherReq& req, sfu_rpc::AddPublisherRsp* rsp) const {
    if (rsp == nullptr) {
        return false;
    }

    rsp->set_success(false);
    rsp->set_udp_port(0);

    if (req.meeting_id().empty() || req.user_id().empty() || !mediaIngress_) {
        return true;
    }

    const bool bound = mediaIngress_->BindPublisher(req.meeting_id(), req.user_id(), req.audio_ssrc(), req.video_ssrc());
    rsp->set_success(bound);
    if (bound) {
        rsp->set_udp_port(mediaIngress_->Port());
    }
    return true;
}

bool RpcService::HandleRemovePublisher(const sfu_rpc::RemovePublisherReq& req, sfu_rpc::RemovePublisherRsp* rsp) const {
    if (rsp == nullptr) {
        return false;
    }

    rsp->set_success(false);
    if (req.meeting_id().empty() || req.user_id().empty() || !mediaIngress_) {
        return true;
    }

    rsp->set_success(mediaIngress_->RemovePublisher(req.meeting_id(), req.user_id()));
    return true;
}

bool RpcService::Dispatch(RpcMethod method, const uint8_t* payload, std::size_t len, std::vector<uint8_t>* responsePayload) const {
    if (responsePayload == nullptr) {
        return false;
    }

    responsePayload->clear();

    switch (method) {
    case RpcMethod::kCreateRoom: {
        sfu_rpc::CreateRoomReq req;
        sfu_rpc::CreateRoomRsp rsp;
        if (payload != nullptr && len > 0) {
            req.ParseFromArray(payload, static_cast<int>(len));
        }
        if (!HandleCreateRoom(req, &rsp)) {
            return false;
        }
        return SerializeMessage(rsp, responsePayload);
    }
    case RpcMethod::kDestroyRoom: {
        sfu_rpc::DestroyRoomReq req;
        sfu_rpc::DestroyRoomRsp rsp;
        if (payload != nullptr && len > 0) {
            req.ParseFromArray(payload, static_cast<int>(len));
        }
        if (!HandleDestroyRoom(req, &rsp)) {
            return false;
        }
        return SerializeMessage(rsp, responsePayload);
    }
    case RpcMethod::kAddPublisher: {
        sfu_rpc::AddPublisherReq req;
        sfu_rpc::AddPublisherRsp rsp;
        if (payload != nullptr && len > 0) {
            req.ParseFromArray(payload, static_cast<int>(len));
        }
        if (!HandleAddPublisher(req, &rsp)) {
            return false;
        }
        return SerializeMessage(rsp, responsePayload);
    }
    case RpcMethod::kRemovePublisher: {
        sfu_rpc::RemovePublisherReq req;
        sfu_rpc::RemovePublisherRsp rsp;
        if (payload != nullptr && len > 0) {
            req.ParseFromArray(payload, static_cast<int>(len));
        }
        if (!HandleRemovePublisher(req, &rsp)) {
            return false;
        }
        return SerializeMessage(rsp, responsePayload);
    }
    default:
        return false;
    }
}

} // namespace sfu
