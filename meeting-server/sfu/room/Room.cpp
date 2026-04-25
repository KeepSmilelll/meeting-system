#include "Room.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace sfu {

Room::Room(std::string meetingId, std::size_t maxPublishers)
    : meetingId_(std::move(meetingId))
    , maxPublishers_(maxPublishers) {}

const std::string& Room::MeetingId() const noexcept {
    return meetingId_;
}

std::size_t Room::MaxPublishers() const noexcept {
    return maxPublishers_;
}

std::size_t Room::PublisherCount() const {
    std::shared_lock lock(mutex_);
    return publishers_.size();
}

std::size_t Room::SubscriberCount() const {
    std::shared_lock lock(mutex_);
    return subscribers_.size();
}

bool Room::AddPublisher(PublisherPtr publisher) {
    if (!publisher || publisher->Empty()) {
        return false;
    }

    const auto userId = publisher->UserId();

    std::unique_lock lock(mutex_);
    const auto it = publishers_.find(userId);
    if (it != publishers_.end()) {
        it->second = std::move(publisher);
        return true;
    }

    if (maxPublishers_ != 0 && publishers_.size() >= maxPublishers_) {
        return false;
    }

    const auto [insertedIt, inserted] = publishers_.emplace(userId, std::move(publisher));
    return inserted && insertedIt->second != nullptr;
}

bool Room::RemovePublisher(const std::string& userId) {
    if (userId.empty()) {
        return false;
    }

    std::unique_lock lock(mutex_);
    return publishers_.erase(userId) > 0;
}

Room::PublisherPtr Room::GetPublisher(const std::string& userId) const {
    if (userId.empty()) {
        return nullptr;
    }

    std::shared_lock lock(mutex_);
    const auto it = publishers_.find(userId);
    return it == publishers_.end() ? nullptr : it->second;
}

bool Room::FindPublisherBySsrc(uint32_t ssrc, PublisherLookup* out) const {
    if (out == nullptr || ssrc == 0) {
        return false;
    }

    std::shared_lock lock(mutex_);
    for (const auto& [userId, publisher] : publishers_) {
        if (!publisher) {
            continue;
        }

        if (publisher->AudioSsrc() == ssrc || publisher->VideoSsrc() == ssrc) {
            out->publisher = publisher;
            out->userId = userId;
            return true;
        }
    }

    return false;
}

std::vector<std::string> Room::GetPublisherIds() const {
    std::shared_lock lock(mutex_);
    return KeysOf(publishers_);
}

std::vector<Room::PublisherPtr> Room::SnapshotPublishers() const {
    std::shared_lock lock(mutex_);
    std::vector<PublisherPtr> publishers;
    publishers.reserve(publishers_.size());
    for (const auto& [userId, publisher] : publishers_) {
        (void)userId;
        publishers.push_back(publisher);
    }
    return publishers;
}

bool Room::AddSubscriber(SubscriberPtr subscriber) {
    if (!subscriber || subscriber->Empty()) {
        return false;
    }

    const auto userId = subscriber->UserId();

    std::unique_lock lock(mutex_);
    const auto it = subscribers_.find(userId);
    if (it != subscribers_.end()) {
        it->second = std::move(subscriber);
        return true;
    }

    const auto [insertedIt, inserted] = subscribers_.emplace(userId, std::move(subscriber));
    return inserted && insertedIt->second != nullptr;
}

bool Room::RemoveSubscriber(const std::string& userId) {
    if (userId.empty()) {
        return false;
    }

    std::unique_lock lock(mutex_);
    return subscribers_.erase(userId) > 0;
}

Room::SubscriberPtr Room::GetSubscriber(const std::string& userId) const {
    if (userId.empty()) {
        return nullptr;
    }

    std::shared_lock lock(mutex_);
    const auto it = subscribers_.find(userId);
    return it == subscribers_.end() ? nullptr : it->second;
}

std::vector<std::string> Room::GetSubscriberIds() const {
    std::shared_lock lock(mutex_);
    return KeysOf(subscribers_);
}

std::vector<Room::SubscriberPtr> Room::SnapshotSubscribers() const {
    std::shared_lock lock(mutex_);
    std::vector<SubscriberPtr> subscribers;
    subscribers.reserve(subscribers_.size());
    for (const auto& [userId, subscriber] : subscribers_) {
        (void)userId;
        subscribers.push_back(subscriber);
    }
    return subscribers;
}

bool Room::RemoveParticipant(const std::string& userId) {
    if (userId.empty()) {
        return false;
    }

    std::unique_lock lock(mutex_);
    const auto pubRemoved = publishers_.erase(userId) > 0;
    const auto subRemoved = subscribers_.erase(userId) > 0;
    return pubRemoved || subRemoved;
}

void Room::Clear() {
    std::unique_lock lock(mutex_);
    publishers_.clear();
    subscribers_.clear();
}

std::vector<std::string> Room::KeysOf(const PublisherMap& values) {
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::string> Room::KeysOf(const SubscriberMap& values) {
    std::vector<std::string> keys;
    keys.reserve(values.size());
    for (const auto& [key, value] : values) {
        (void)value;
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

} // namespace sfu



