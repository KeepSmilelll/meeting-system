#!/usr/bin/env bash
set -euo pipefail
protoc --proto_path=. --go_out=../signaling/protocol/pb --go_opt=paths=source_relative signaling.proto

