# PROTO WORKFLOW

Minimal protocol maintenance flow for `meeting-server/proto`.

## 1) Update protocol definitions

- Edit `signaling.proto` and/or `sfu_rpc.proto`.
- Keep message fields backward compatible when possible. Prefer add-only changes.
- `AuthLoginReq.password_hash` is for password-based login only.
- `AuthLoginReq.resume_token` is for cached session restore; clients should send one or the other depending on the login path.
- Keep signal type ranges consistent with `signaling/protocol/signal_type.go`:
  - auth: `0x01xx`
  - meeting: `0x02xx`
  - media: `0x03xx`
  - chat: `0x04xx`
  - file: `0x05xx`

### Leave lifecycle contract

- `MeetLeaveReq` / `MeetLeaveRsp` remain the client-facing leave handshake. They do not need extra fields for room teardown.
- `MeetParticipantLeaveNotify.reason` is the only leave reason channel exposed to clients. Use it for user-visible leave causes such as active leave, kick, or network drop.
- Room destruction, host transfer, SFU publisher cleanup, and Redis/MySQL state reconciliation are server-side lifecycle steps. Do not add a separate client protocol for `destroy-room` unless a client-visible transition is required.
- If the current leave flow can be expressed with the existing messages, keep the proto unchanged and document the lifecycle here instead of adding fields.

## 2) Toolchain requirements

- Go protobuf plugin: `protoc-gen-go`
- Default `protoc` for this repo: `D:\go-env\protoc-3.19.1\bin\protoc.exe`
- Qt client protobuf generation must use `--cpp_out=lite:`
- Qt client protobuf runtime must stay aligned with the generated code. The current repo is validated against the local OpenCV protobuf headers/libs.

## 3) One-shot generation

### PowerShell

```powershell
cd D:\meeting\meeting-server\proto
.\generate.ps1
```

To override the compiler path:

```powershell
cd D:\meeting\meeting-server\proto
.\generate.ps1 -ProtocPath D:\go-env\protoc-3.19.1\bin\protoc.exe
```

You can also set `PROTOC_BIN` or `PROTOC_PATH` before running the script.

### Bash

```bash
cd D:/meeting/meeting-server/proto
./generate.sh
```

To override the compiler path:

```bash
cd D:/meeting/meeting-server/proto
./generate.sh D:/go-env/protoc-3.19.1/bin/protoc.exe
```

You can also set `PROTOC_BIN` or `PROTOC_PATH` before running the script.

## 4) Generated outputs

- Go: `D:\meeting\meeting-server\signaling\protocol\pb`
- SFU C++: `D:\meeting\meeting-server\sfu\proto\pb`
- Qt client C++ lite: `D:\meeting\plasma-hawking\proto\pb`

The Go generator writes both `signaling.proto` and `sfu_rpc.proto` into the shared Go package path.
The Qt client generator only emits `signaling.proto`, and uses lite mode to avoid dragging in the full protobuf runtime.

## 5) Validation

### Go signaling

```powershell
cd D:\meeting\meeting-server\signaling
go test ./...
```

### Qt client

```powershell
cmake -S D:\meeting\plasma-hawking -B D:\meeting\plasma-hawking\build_phase2_check
cmake --build D:\meeting\plasma-hawking\build_phase2_check --config Debug --target meeting_client
```

### SFU core

```powershell
cmake -S D:\meeting\meeting-server\sfu -B D:\meeting\meeting-server\sfu\build_sfu_check
cmake --build D:\meeting\meeting-server\sfu\build_sfu_check --config Debug --target sfu_core_tests
ctest --test-dir D:\meeting\meeting-server\sfu\build_sfu_check -C Debug --output-on-failure
```

## 6) Common failures

- `protoc-gen-go not found`: install it with `go install google.golang.org/protobuf/cmd/protoc-gen-go@latest` and make sure `GOPATH\bin` is on `PATH`.
- `protoc` not found: pass `-ProtocPath`/first script argument, or set `PROTOC_BIN` / `PROTOC_PATH`.
- Qt build links against a newer protobuf runtime and fails on `ZeroFieldsBase`: regenerate the Qt C++ output with `protoc 3.19.1` and keep `--cpp_out=lite:`.
- `protoc` version mismatch errors in generated headers: regenerate with the same compiler version that the project expects.
- Missing protobuf headers in Qt CMake: verify the client build points at the local protobuf include directory and library set documented in the Qt project.

