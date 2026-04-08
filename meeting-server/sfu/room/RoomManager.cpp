#include "RoomManager.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace sfu {

bool RoomManager::CreateRoom(const std::string& meetingId, std::size_t maxPublishers) {
    if (meetingId.empty() || maxPublishers == 0) {
        return false;
    }

    std::unique_lock lock(mutex_);
    const auto [it, inserted] = rooms_.emplace(meetingId, std::make_shared<Room>(meetingId, maxPublishers));
    return inserted && it->second != nullptr;
}

bool RoomManager::DestroyRoom(const std::string& meetingId) {
    if (meetingId.empty()) {
        return false;
    }

    std::unique_lock lock(mutex_);
    const auto it = rooms_.find(meetingId);
    if (it == rooms_.end()) {
        return false;
    }

    it->second->Clear();
    rooms_.erase(it);
    return true;
}

bool RoomManager::RemoveParticipant(const std::string& meetingId, const std::string& userId) {
    if (meetingId.empty() || userId.empty()) {
        return false;
    }

    std::shared_lock lock(mutex_);
    const auto it = rooms_.find(meetingId);
    if (it == rooms_.end() || !it->second) {
        return false;
    }

    return it->second->RemoveParticipant(userId);
}

Room* RoomManager::GetRoom(const std::string& meetingId) const {
    if (meetingId.empty()) {
        return nullptr;
    }

    std::shared_lock lock(mutex_);
    const auto it = rooms_.find(meetingId);
    return it == rooms_.end() ? nullptr : it->second.get();
}

RoomManager::RoomPtr RoomManager::GetRoomShared(const std::string& meetingId) const {
    if (meetingId.empty()) {
        return nullptr;
    }

    std::shared_lock lock(mutex_);
    const auto it = rooms_.find(meetingId);
    return it == rooms_.end() ? nullptr : it->second;
}

bool RoomManager::FindPublisherBySsrc(uint32_t ssrc, PublisherLocation* out) const {
    if (out == nullptr || ssrc == 0) {
        return false;
    }

    std::shared_lock lock(mutex_);
    for (const auto& [meetingId, room] : rooms_) {
        if (!room) {
            continue;
        }

        Room::PublisherLookup publisher;
        if (room->FindPublisherBySsrc(ssrc, &publisher)) {
            out->room = room;
            out->publisher = std::move(publisher);
            out->meetingId = meetingId;
            return true;
        }
    }

    return false;
}

bool RoomManager::HasRoom(const std::string& meetingId) const {
    if (meetingId.empty()) {
        return false;
    }

    std::shared_lock lock(mutex_);
    return rooms_.find(meetingId) != rooms_.end();
}

std::size_t RoomManager::RoomCount() const {
    std::shared_lock lock(mutex_);
    return rooms_.size();
}

std::size_t RoomManager::PublisherCount() const {
    std::shared_lock lock(mutex_);
    std::size_t count = 0;
    for (const auto& [meetingId, room] : rooms_) {
        (void)meetingId;
        if (!room) {
            continue;
        }
        count += room->PublisherCount();
    }
    return count;
}

std::vector<std::string> RoomManager::GetRoomIds() const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> ids;
    ids.reserve(rooms_.size());
    for (const auto& [roomId, room] : rooms_) {
        (void)room;
        ids.push_back(roomId);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

} // namespace sfu
