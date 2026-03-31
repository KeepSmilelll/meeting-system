#include "room/Publisher.h"
#include "room/Room.h"
#include "room/RoomManager.h"
#include "rtp/RtpParser.h"
#include "rtp/RtpRouter.h"

#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

int main() {
    sfu::RtpRouter router(8);
    if (!router.RegisterPublisher(0x11111111U)) {
        std::cerr << "RegisterPublisher failed\n";
        return 1;
    }

    bool forwarded = false;
    std::vector<uint8_t> forwardedPacket;
    if (!router.AddSubscriber(0x11111111U, 0x22222222U,
        [&forwarded, &forwardedPacket](const uint8_t* data, std::size_t len) {
            forwarded = true;
            forwardedPacket.assign(data, data + len);
        })) {
        std::cerr << "AddSubscriber failed\n";
        return 1;
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
        return 1;
    }

    if (!forwarded || forwardedPacket.size() != packet.size() ||
        forwardedPacket[8] != 0x22 || forwardedPacket[9] != 0x22 ||
        forwardedPacket[10] != 0x22 || forwardedPacket[11] != 0x22) {
        std::cerr << "Forwarded RTP packet mismatch\n";
        return 1;
    }

    sfu::RoomManager manager;
    if (!manager.CreateRoom("room-lookup", 2)) {
        std::cerr << "CreateRoom failed\n";
        return 1;
    }
    auto room = manager.GetRoomShared("room-lookup");
    if (!room) {
        std::cerr << "GetRoomShared failed\n";
        return 1;
    }
    if (!room->AddPublisher(std::make_shared<sfu::Publisher>("alice", 1234, 5678))) {
        std::cerr << "AddPublisher failed\n";
        return 1;
    }

    sfu::Room::PublisherLookup roomLookup;
    if (!room->FindPublisherBySsrc(1234, &roomLookup) || !roomLookup.publisher || roomLookup.userId != "alice") {
        std::cerr << "Room::FindPublisherBySsrc failed\n";
        return 1;
    }

    sfu::RoomManager::PublisherLocation managerLookup;
    if (!manager.FindPublisherBySsrc(5678, &managerLookup) || !managerLookup.room || managerLookup.meetingId != "room-lookup") {
        std::cerr << "RoomManager::FindPublisherBySsrc failed\n";
        return 1;
    }

    std::cout << "test_rtp_router passed\n";
    return 0;
}
