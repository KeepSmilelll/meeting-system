#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Publisher.h"
#include "Subscriber.h"

namespace sfu {

class Room final {
public:
    using PublisherPtr = std::shared_ptr<Publisher>;
    using SubscriberPtr = std::shared_ptr<Subscriber>;

    struct PublisherLookup {
        PublisherPtr publisher;
        std::string userId;
    };

    Room(std::string meetingId, std::size_t maxPublishers);

    const std::string& MeetingId() const noexcept;
    std::size_t MaxPublishers() const noexcept;

    std::size_t PublisherCount() const;
    std::size_t SubscriberCount() const;

    bool AddPublisher(PublisherPtr publisher);
    bool RemovePublisher(const std::string& userId);
    PublisherPtr GetPublisher(const std::string& userId) const;
    bool FindPublisherBySsrc(uint32_t ssrc, PublisherLookup* out) const;
    std::vector<std::string> GetPublisherIds() const;
    std::vector<PublisherPtr> SnapshotPublishers() const;

    bool AddSubscriber(SubscriberPtr subscriber);
    bool RemoveSubscriber(const std::string& userId);
    SubscriberPtr GetSubscriber(const std::string& userId) const;
    std::vector<std::string> GetSubscriberIds() const;
    std::vector<SubscriberPtr> SnapshotSubscribers() const;

    bool RemoveParticipant(const std::string& userId);

    void Clear();

private:
    using PublisherMap = std::unordered_map<std::string, PublisherPtr>;
    using SubscriberMap = std::unordered_map<std::string, SubscriberPtr>;

    static std::vector<std::string> KeysOf(const PublisherMap& values);
    static std::vector<std::string> KeysOf(const SubscriberMap& values);

    mutable std::shared_mutex mutex_;
    std::string meetingId_;
    std::size_t maxPublishers_{0};
    PublisherMap publishers_;
    SubscriberMap subscribers_;
};

} // namespace sfu
