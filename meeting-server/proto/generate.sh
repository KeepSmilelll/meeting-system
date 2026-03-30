#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
GO_OUT="$SCRIPT_DIR/../signaling/protocol/pb"
SFU_CPP_OUT="$SCRIPT_DIR/../sfu/proto/pb"
CLIENT_CPP_OUT="$SCRIPT_DIR/../../plasma-hawking/proto/pb"

mkdir -p "$GO_OUT" "$SFU_CPP_OUT" "$CLIENT_CPP_OUT"

PROTO_FILES=("signaling.proto" "sfu_rpc.proto")

protoc --proto_path="$SCRIPT_DIR" \
  --go_out="$GO_OUT" --go_opt=paths=source_relative \
  "${PROTO_FILES[@]}"

protoc --proto_path="$SCRIPT_DIR" \
  --cpp_out="$SFU_CPP_OUT" \
  "${PROTO_FILES[@]}"

protoc --proto_path="$SCRIPT_DIR" \
  --cpp_out="$CLIENT_CPP_OUT" \
  signaling.proto

echo "Generated Go pb -> $GO_OUT"
echo "Generated SFU C++ pb -> $SFU_CPP_OUT"
echo "Generated Client C++ pb -> $CLIENT_CPP_OUT"
