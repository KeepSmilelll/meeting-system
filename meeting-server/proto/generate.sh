#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GO_OUT="$SCRIPT_DIR/../signaling/protocol/pb"
SFU_CPP_OUT="$SCRIPT_DIR/../sfu/proto/pb"
CLIENT_CPP_OUT="$SCRIPT_DIR/../../plasma-hawking/proto/pb"
PROTO_FILES=(signaling.proto sfu_rpc.proto)

resolve_protoc() {
  local candidate="${1:-}"

  if [[ -n "$candidate" ]]; then
    if [[ -f "$candidate" || -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  fi

  if [[ -n "${PROTOC_BIN:-}" ]]; then
    candidate="$PROTOC_BIN"
    if [[ -f "$candidate" || -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  fi

  if [[ -n "${PROTOC_PATH:-}" ]]; then
    candidate="$PROTOC_PATH"
    if [[ -f "$candidate" || -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
    if command -v "$candidate" >/dev/null 2>&1; then
      command -v "$candidate"
      return 0
    fi
  fi

  candidate="D:/go-env/protoc-3.19.1/bin/protoc.exe"
  if [[ -f "$candidate" || -x "$candidate" ]]; then
    printf '%s\n' "$candidate"
    return 0
  fi

  if command -v protoc >/dev/null 2>&1; then
    command -v protoc
    return 0
  fi

  printf 'Unable to locate protoc. Pass the path as the first argument, set PROTOC_BIN/PROTOC_PATH, or install protoc on PATH.\n' >&2
  return 1
}

PROTOC_BIN="$(resolve_protoc "${1:-}")"

if ! command -v protoc-gen-go >/dev/null 2>&1; then
  cat >&2 <<'EOF'
protoc-gen-go not found. Install it with:
  go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
EOF
  exit 1
fi

mkdir -p "$GO_OUT" "$SFU_CPP_OUT" "$CLIENT_CPP_OUT"

for file in "${PROTO_FILES[@]}"; do
  if [[ ! -f "$SCRIPT_DIR/$file" ]]; then
    printf 'Missing proto file: %s\n' "$SCRIPT_DIR/$file" >&2
    exit 1
  fi
done

if [[ ! -f "$SCRIPT_DIR/signaling.proto" ]]; then
  printf 'Missing client proto file: %s\n' "$SCRIPT_DIR/signaling.proto" >&2
  exit 1
fi

"$PROTOC_BIN" --proto_path="$SCRIPT_DIR" --go_out="$GO_OUT" --go_opt=paths=source_relative "${PROTO_FILES[@]}"
"$PROTOC_BIN" --proto_path="$SCRIPT_DIR" --cpp_out="$SFU_CPP_OUT" "${PROTO_FILES[@]}"
"$PROTOC_BIN" --proto_path="$SCRIPT_DIR" --cpp_out=lite:"$CLIENT_CPP_OUT" "$SCRIPT_DIR/signaling.proto"

echo "Generated Go pb -> $GO_OUT"
echo "Generated SFU C++ pb -> $SFU_CPP_OUT"
echo "Generated Qt lite C++ pb -> $CLIENT_CPP_OUT"
echo "Using protoc -> $PROTOC_BIN"
