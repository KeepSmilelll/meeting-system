# Qt Client (Phase 2 Skeleton)

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Scope

- CMake + Qt Quick project skeleton
- `MeetingController` exposed to QML
- Basic meeting lifecycle UI actions (create/join/leave, audio/video toggle)
- Signaling frame codec utility (`SignalProtocol`) for Phase 0 protocol reuse

## Next

- Replace local mock logic in `MeetingController` with real `SignalingClient` TCP implementation
- Integrate protobuf request/response serialization
- Add reconnect, token restore, and meeting state sync
