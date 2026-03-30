# Development Progress (By Phase)

## Phase 0 - Shared Protocol Layer

- [x] Added `proto/signaling.proto` based on interface docs.
- [x] Added `proto/generate.ps1` for Go protobuf generation.
- [x] Implemented frame codec on Go side: `signaling/protocol/frame.go`.
- [x] Implemented frame codec utility on client side: `client/src/net/signaling/SignalProtocol.*`.

## Phase 1 - Go Signaling Server Skeleton

- [x] Initialized Go module and directory layout.
- [x] Implemented TCP listener and concurrent session model.
- [x] Implemented session manager with single-login kick behavior.
- [x] Implemented router and core handlers:
  - [x] Auth login/logout/heartbeat
  - [x] Meeting create/join/leave
  - [x] Chat send + room broadcast
- [x] Added in-memory store and basic unit tests.

## Phase 2 - Qt Client Skeleton

- [x] Added Qt CMake project skeleton and QML module.
- [x] Added `MeetingController` with phase-2 placeholder logic.
- [x] Added basic meeting UI page with create/join/leave/mute actions.

## Upcoming (Phase 3+)

- [ ] Replace JSON payload path with generated protobuf payload path.
- [ ] Add Redis/MySQL repositories and persistence.
- [ ] Implement SFU RPC integration and media negotiation forwarding.
- [ ] Implement real `SignalingClient` in Qt client and state recovery.
