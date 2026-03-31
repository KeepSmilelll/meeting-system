#include "room/Publisher.h"
#include "room/Room.h"
#include "room/RoomManager.h"
#include "room/Subscriber.h"
#include "rtp/NackBuffer.h"
#include "rtp/RtpParser.h"
#include "rtp/RtpRouter.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

namespace {

bool TestRtpParserValidPacket() {
    std::vector<uint8_t> packet = {
        0x80, 0x60,       // V=2, PT=96
        0x00, 0x2A,       // Seq=42
        0x00, 0x00, 0x00, 0x64, // TS=100
        0x11, 0x11, 0x11, 0x11, // SSRC=0x11111111
        0xAA, 0xBB, 0xCC
    };

    sfu::ParsedRtpPacket parsed;
    sfu::RtpParser parser;
    if (!parser.Parse(packet.data(), packet.size(), &parsed)) {
        std::cerr << "RtpParser should parse valid RTP packet\n";
        return false;
    }

    return parsed.header.version == 2 &&
           parsed.header.payloadType == 96 &&
           parsed.header.sequence == 42 &&
           parsed.header.ssrc == 0x11111111U &&
           parsed.payloadSize == 3;
}

bool TestRtpParserRejectInvalidVersion() {
    std::vector<uint8_t> packet = {
        0x40, 0x60, // V=1 (invalid)
        0x00, 0x01,
        0x00, 0x00, 0x00, 0x01,
        0x11, 0x11, 0x11, 0x11,
    };

    sfu::ParsedRtpPacket parsed;
    sfu::RtpParser parser;
    return !parser.Parse(packet.data(), packet.size(), &parsed);
}

bool TestNackBufferCapacity() {
    sfu::NackBuffer buffer(2);

    const std::vector<uint8_t> p1 = {1, 2, 3};
    const std::vector<uint8_t> p2 = {4, 5, 6};
    const std::vector<uint8_t> p3 = {7, 8, 9};

    buffer.Store(1, p1.data(), p1.size());
    buffer.Store(2, p2.data(), p2.size());
    buffer.Store(3, p3.data(), p3.size()); // should evict seq=1

    std::vector<uint8_t> out;
    if (buffer.Get(1, &out)) {
        std::cerr << "Oldest packet should be evicted when capacity exceeded\n";
        return false;
    }

    return buffer.Get(2, &out) && buffer.Get(3, &out) && buffer.Size() == 2;
}

bool TestRtpRouterForwardAndRetransmit() {
    sfu::RtpRouter router(8);
    if (!router.RegisterPublisher(0x11111111U)) {
        std::cerr << "RegisterPublisher failed\n";
        return false;
    }

    bool forwarded = false;
    std::vector<uint8_t> forwardedPacket;

    if (!router.AddSubscriber(0x11111111U, 0x22222222U,
        [&forwarded, &forwardedPacket](const uint8_t* data, std::size_t len) {
            forwarded = true;
            forwardedPacket.assign(data, data + len);
        })) {
        std::cerr << "AddSubscriber failed\n";
        return false;
    }

    std::vector<uint8_t> packet = {
        0x80, 0x60,
        0x00, 0x2A,
        0x00, 0x00, 0x00, 0x64,
        0x11, 0x11, 0x11, 0x11,
        0xAB, 0xCD
    };

    if (!router.Route(packet.data(), packet.size())) {
        std::cerr << "Route failed\n";
        return false;
    }

    if (!forwarded || forwardedPacket.size() != packet.size()) {
        std::cerr << "Forward callback not invoked as expected\n";
        return false;
    }

    if (forwardedPacket[8] != 0x22 || forwardedPacket[9] != 0x22 ||
        forwardedPacket[10] != 0x22 || forwardedPacket[11] != 0x22) {
        std::cerr << "Forwarded packet SSRC rewrite mismatch\n";
        return false;
    }

    std::vector<uint8_t> retransmit;
    if (!router.GetRetransmitPacket(0x11111111U, 42, &retransmit)) {
        std::cerr << "GetRetransmitPacket failed\n";
        return false;
    }

    return retransmit == packet;
}

bool TestRoomManagerLifecycle() {
    sfu::RoomManager manager;

    if (manager.CreateRoom("room-1", 2) != true) {
        std::cerr << "CreateRoom should succeed\n";
        return false;
    }
    if (manager.CreateRoom("room-1", 2)) {
        std::cerr << "Duplicate CreateRoom should fail\n";
        return false;
    }
    if (!manager.HasRoom("room-1") || manager.RoomCount() != 1) {
        std::cerr << "RoomManager should report created room\n";
        return false;
    }

    sfu::Room* room = manager.GetRoom("room-1");
    if (room == nullptr) {
        std::cerr << "GetRoom should return created room\n";
        return false;
    }
    if (room->MeetingId() != "room-1" || room->MaxPublishers() != 2) {
        std::cerr << "Room metadata mismatch\n";
        return false;
    }

    const auto sharedRoom = manager.GetRoomShared("room-1");
    if (!sharedRoom) {
        std::cerr << "GetRoomShared should return room handle\n";
        return false;
    }

    if (!manager.DestroyRoom("room-1")) {
        std::cerr << "DestroyRoom should succeed\n";
        return false;
    }
    if (manager.HasRoom("room-1") || manager.RoomCount() != 0 || manager.GetRoom("room-1") != nullptr) {
        std::cerr << "DestroyRoom should remove room from manager\n";
        return false;
    }

    if (sharedRoom->PublisherCount() != 0 || sharedRoom->SubscriberCount() != 0) {
        std::cerr << "DestroyRoom should clear room contents before erase\n";
        return false;
    }

    return !manager.DestroyRoom("room-1");
}

bool TestRoomLookupBySsrc() {
    sfu::RoomManager manager;
    if (!manager.CreateRoom("room-lookup", 2)) {
        std::cerr << "CreateRoom should succeed for lookup test\n";
        return false;
    }

    auto room = manager.GetRoomShared("room-lookup");
    if (!room) {
        std::cerr << "GetRoomShared should return lookup room\n";
        return false;
    }

    auto publisher = std::make_shared<sfu::Publisher>("alice", 1234, 5678);
    if (!room->AddPublisher(publisher)) {
        std::cerr << "AddPublisher should succeed\n";
        return false;
    }

    sfu::Room::PublisherLookup roomLookup;
    if (!room->FindPublisherBySsrc(1234, &roomLookup) || !roomLookup.publisher || roomLookup.userId != "alice") {
        std::cerr << "Room::FindPublisherBySsrc should resolve the audio source\n";
        return false;
    }

    sfu::RoomManager::PublisherLocation managerLookup;
    if (!manager.FindPublisherBySsrc(5678, &managerLookup) ||
        !managerLookup.room || !managerLookup.publisher.publisher ||
        managerLookup.meetingId != "room-lookup" ||
        managerLookup.publisher.userId != "alice") {
        std::cerr << "RoomManager::FindPublisherBySsrc should locate the publisher\n";
        return false;
    }

    const auto publishers = room->SnapshotPublishers();
    if (publishers.size() != 1 || publishers.front() == nullptr || publishers.front()->UserId() != "alice") {
        std::cerr << "SnapshotPublishers should capture current publishers\n";
        return false;
    }

    return true;
}

bool TestRoomLeaveCleanup() {
    sfu::Room room("room-2", 2);

    auto publisher = std::make_shared<sfu::Publisher>("alice", 11, 22);
    auto subscriber = std::make_shared<sfu::Subscriber>("alice", "udp://alice", 33, 44);
    auto otherSubscriber = std::make_shared<sfu::Subscriber>("bob", "udp://bob", 55, 66);

    if (!room.AddPublisher(publisher) || !room.AddSubscriber(subscriber) || !room.AddSubscriber(otherSubscriber)) {
        std::cerr << "Room add operations should succeed\n";
        return false;
    }

    if (!room.RemoveParticipant("alice")) {
        std::cerr << "RemoveParticipant should remove matching publisher/subscriber\n";
        return false;
    }
    if (room.GetPublisher("alice") != nullptr || room.GetSubscriber("alice") != nullptr) {
        std::cerr << "Removed participant should no longer be present\n";
        return false;
    }
    if (room.PublisherCount() != 0 || room.SubscriberCount() != 1) {
        std::cerr << "Counts should reflect participant removal\n";
        return false;
    }

    if (room.RemoveParticipant("alice")) {
        std::cerr << "Removing a non-existent participant should fail\n";
        return false;
    }

    sfu::RoomManager manager;
    if (!manager.CreateRoom("room-3", 2)) {
        std::cerr << "CreateRoom for leave cleanup test should succeed\n";
        return false;
    }
    auto managedRoom = manager.GetRoomShared("room-3");
    if (!managedRoom) {
        std::cerr << "GetRoomShared should return managed room\n";
        return false;
    }
    if (!managedRoom->AddPublisher(std::make_shared<sfu::Publisher>("carol", 77, 88)) ||
        !managedRoom->AddSubscriber(std::make_shared<sfu::Subscriber>("carol", "udp://carol", 99, 100))) {
        std::cerr << "Managed room population should succeed\n";
        return false;
    }

    if (!manager.RemoveParticipant("room-3", "carol")) {
        std::cerr << "RoomManager::RemoveParticipant should delegate to room cleanup\n";
        return false;
    }
    if (managedRoom->PublisherCount() != 0 || managedRoom->SubscriberCount() != 0) {
        std::cerr << "RoomManager::RemoveParticipant should clear both collections\n";
        return false;
    }

    return true;
}

} // namespace

int main() {
    if (!TestRtpParserValidPacket()) {
        return 1;
    }
    if (!TestRtpParserRejectInvalidVersion()) {
        return 1;
    }
    if (!TestNackBufferCapacity()) {
        return 1;
    }
    if (!TestRtpRouterForwardAndRetransmit()) {
        return 1;
    }
    if (!TestRoomManagerLifecycle()) {
        return 1;
    }
    if (!TestRoomLookupBySsrc()) {
        return 1;
    }
    if (!TestRoomLeaveCleanup()) {
        return 1;
    }

    std::cout << "sfu_core_tests passed\n";
    return 0;
}
