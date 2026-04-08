#include "bwe/BandwidthEstimator.h"
#include "room/Publisher.h"
#include "room/Room.h"
#include "room/RoomManager.h"
#include "room/Subscriber.h"
#include "rtp/NackBuffer.h"
#include "rtp/RtcpHandler.h"
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

bool TestRtcpHandlerParsesReceiverReports() {
    const std::vector<uint8_t> packet = {
        0x81, 0xC9, 0x00, 0x07,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x40, 0x00, 0x00, 0x02,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x01, 0xE0,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };

    sfu::RtcpHandler handler;
    const auto summaries = handler.ParseCompound(packet.data(), packet.size());
    if (summaries.size() != 1 || summaries.front().type != sfu::RtcpType::RR || summaries.front().senderSsrc != 0x22222222U) {
        std::cerr << "RtcpHandler should parse compound RR packet\n";
        return false;
    }

    const auto reports = handler.ParseReceptionReports(packet.data(), packet.size());
    if (reports.size() != 1) {
        std::cerr << "RtcpHandler should extract one report block\n";
        return false;
    }

    return reports.front().reporterSsrc == 0x22222222U &&
           reports.front().sourceSsrc == 0x11111111U &&
           reports.front().fractionLost == 0x40 &&
           reports.front().jitter == 480U;
}

bool TestRtcpHandlerParsesSignedCumulativeLost() {
    const std::vector<uint8_t> packet = {
        0x81, 0xC9, 0x00, 0x07,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x01, 0xFF, 0xFF, 0xFF,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x01, 0xE0,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };

    sfu::RtcpHandler handler;
    const auto reports = handler.ParseReceptionReports(packet.data(), packet.size());
    if (reports.size() != 1) {
        std::cerr << "RtcpHandler should parse one report for signed loss test\n";
        return false;
    }

    return reports.front().fractionLost == 0x01 &&
           reports.front().cumulativeLost == -1;
}

bool TestRtcpHandlerParsesSenderReportBlocks() {
    const std::vector<uint8_t> packet = {
        0x81, 0xC8, 0x00, 0x0C,
        0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x11, 0x11, 0x11, 0x11,
        0x20, 0x00, 0x00, 0x01,
        0x00, 0x00, 0x10, 0x00,
        0x00, 0x00, 0x03, 0x84,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };

    sfu::RtcpHandler handler;
    const auto reports = handler.ParseReceptionReports(packet.data(), packet.size());
    if (reports.size() != 1) {
        std::cerr << "RtcpHandler should parse one SR report block\n";
        return false;
    }

    return reports.front().packetType == sfu::RtcpType::SR &&
           reports.front().reporterSsrc == 0x22222222U &&
           reports.front().sourceSsrc == 0x11111111U &&
           reports.front().fractionLost == 0x20 &&
           reports.front().cumulativeLost == 1 &&
           reports.front().jitter == 900U;
}

bool TestRtcpHandlerParsesSenderReports() {
    const std::vector<uint8_t> packet = {
        0x80, 0xC8, 0x00, 0x06,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x22, 0x33, 0x44,
        0x55, 0x66, 0x77, 0x88,
        0x00, 0x00, 0x03, 0xC0,
        0x00, 0x00, 0x00, 0x05,
        0x00, 0x00, 0x01, 0x80,
    };

    sfu::RtcpHandler handler;
    const auto reports = handler.ParseSenderReports(packet.data(), packet.size());
    if (reports.size() != 1) {
        std::cerr << "RtcpHandler should parse one sender report\n";
        return false;
    }

    return reports.front().senderSsrc == 0x22222222U &&
           reports.front().ntpTimestamp == 0x1122334455667788ULL &&
           reports.front().rtpTimestamp == 0x000003C0U &&
           reports.front().packetCount == 5U &&
           reports.front().octetCount == 0x180U;
}

bool TestRtcpHandlerParsesNackFeedback() {
    const std::vector<uint8_t> packet = {
        0x81, 0xCD, 0x00, 0x03,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
        0x00, 0x2A, 0x00, 0x05,
    };

    sfu::RtcpHandler handler;
    const auto nacks = handler.ParseNackFeedback(packet.data(), packet.size());
    if (nacks.size() != 1) {
        std::cerr << "RtcpHandler should parse one NACK packet\n";
        return false;
    }

    if (nacks.front().senderSsrc != 0x22222222U || nacks.front().mediaSsrc != 0x11111111U) {
        std::cerr << "RtcpHandler parsed NACK SSRC fields incorrectly\n";
        return false;
    }

    const std::vector<uint16_t> expected = {42U, 43U, 45U};
    return nacks.front().lostSequences == expected;
}

bool TestRtcpHandlerParsesPliFeedback() {
    const std::vector<uint8_t> packet = {
        0x81, 0xCE, 0x00, 0x02,
        0x22, 0x22, 0x22, 0x22,
        0x11, 0x11, 0x11, 0x11,
    };

    sfu::RtcpHandler handler;
    const auto plis = handler.ParsePliFeedback(packet.data(), packet.size());
    if (plis.size() != 1) {
        std::cerr << "RtcpHandler should parse one PLI packet\n";
        return false;
    }

    return plis.front().senderSsrc == 0x22222222U &&
           plis.front().mediaSsrc == 0x11111111U;
}

bool TestRtcpHandlerParsesRembFeedback() {
    const std::vector<uint8_t> packet = {
        0x8F, 0xCE, 0x00, 0x05,
        0x22, 0x22, 0x22, 0x22,
        0x00, 0x00, 0x00, 0x00,
        'R', 'E', 'M', 'B',
        0x01,
        0x03, 0x0D, 0x40,
        0x11, 0x11, 0x11, 0x11,
    };

    sfu::RtcpHandler handler;
    const auto rembs = handler.ParseRembFeedback(packet.data(), packet.size());
    if (rembs.size() != 1) {
        std::cerr << "RtcpHandler should parse one REMB packet\n";
        return false;
    }

    return rembs.front().senderSsrc == 0x22222222U &&
           rembs.front().mediaSsrc == 0U &&
           rembs.front().bitrateExp == 0U &&
           rembs.front().bitrateMantissa == 200000U &&
           rembs.front().bitrateBps == 200000U &&
           rembs.front().targetSsrcs.size() == 1 &&
           rembs.front().targetSsrcs.front() == 0x11111111U;
}

bool TestBandwidthEstimatorAdaptsToRttAndLoss() {
    sfu::bwe::BandwidthEstimator estimator(150U, 2500U, 1000U);

    const auto good = estimator.Update({0.01F, 80U, 20U, 1200U});
    if (good <= 1000U) {
        std::cerr << "BWE should increase bitrate under good conditions\n";
        return false;
    }

    const auto degraded = estimator.Update({0.20F, 600U, 140U, 1200U});
    if (degraded >= good) {
        std::cerr << "BWE should decrease bitrate when RTT/loss worsen\n";
        return false;
    }

    const auto floor = estimator.Update({0.35F, 900U, 220U, 0U});
    if (floor < 150U) {
        std::cerr << "BWE should clamp to configured floor\n";
        return false;
    }

    uint32_t recovered = floor;
    for (int i = 0; i < 8; ++i) {
        recovered = estimator.Update({0.00F, 60U, 10U, 2000U});
    }
    if (recovered <= floor) {
        std::cerr << "BWE should recover upward under sustained good conditions\n";
        return false;
    }

    return recovered > degraded;
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
    if (!TestRtcpHandlerParsesReceiverReports()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesSignedCumulativeLost()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesSenderReportBlocks()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesSenderReports()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesNackFeedback()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesPliFeedback()) {
        return 1;
    }
    if (!TestRtcpHandlerParsesRembFeedback()) {
        return 1;
    }
    if (!TestBandwidthEstimatorAdaptsToRttAndLoss()) {
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
