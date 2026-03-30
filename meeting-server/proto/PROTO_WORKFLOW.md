# PROTO WORKFLOW

Minimal protocol maintenance flow for `meeting-server/proto`.

## 1) Update protocol definitions

- Edit `signaling.proto` and/or `sfu_rpc.proto`.
- Keep message fields backward compatible (add-only when possible).
- Keep signal type ranges consistent with `signaling/protocol/signal_type.go`:
  - auth: `0x01xx`
  - meeting: `0x02xx`
  - media: `0x03xx`
  - chat: `0x04xx`
  - file: `0x05xx`

## 2) Generate code

### PowerShell

```powershell
cd D:\meeting\meeting-server\proto
./generate.ps1
```

### Bash

```bash
cd D:/meeting/meeting-server/proto
./generate.sh
```

## 3) Sync generated outputs

Expected generated targets:

- Go: `D:\meeting\meeting-server\signaling\protocol\pb`
- SFU C++: `D:\meeting\meeting-server\sfu\proto\pb`
- Qt client C++: `D:\meeting\plasma-hawking\proto\pb`

After generation, make sure changed files are included in commit (proto + generated code).

## 4) Validate

### Go signaling

```powershell
cd D:\meeting\meeting-server\signaling
go test ./...
```

### Optional checks

- Build SFU target (when C++ toolchain is available).
- Build Qt client target (when Qt + protobuf C++ toolchain is available).
