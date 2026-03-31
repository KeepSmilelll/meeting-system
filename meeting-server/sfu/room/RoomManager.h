#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Room.h"

namespace sfu {

class RoomManager final {
public:
    using RoomPtr = std::shared_ptr<Room>;

    struct PublisherLocation {
        RoomPtr room;
        Room::PublisherLookup publisher;
        std::string meetingId;
    };

    bool CreateRoom(const std::string& meetingId, std::size_t maxPublishers);
    bool DestroyRoom(const std::string& meetingId);
    bool RemoveParticipant(const std::string& meetingId, const std::string& userId);

    Room* GetRoom(const std::string& meetingId) const;
    RoomPtr GetRoomShared(const std::string& meetingId) const;

    bool FindPublisherBySsrc(uint32_t ssrc, PublisherLocation* out) const;

    bool HasRoom(const std::string& meetingId) const;
    std::size_t RoomCount() const;
    std::vector<std::string> GetRoomIds() const;

private:
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, RoomPtr> rooms_;
};

} // namespace sfu
