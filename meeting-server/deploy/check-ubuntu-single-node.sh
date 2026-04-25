#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

echo "== docker compose ps =="
docker compose ps

echo
echo "== signaling tcp check =="
timeout 3 bash -c '</dev/tcp/127.0.0.1/8443' \
  && echo "signaling: tcp 8443 reachable" \
  || echo "signaling: tcp 8443 not reachable"

echo
echo "== recent logs =="
docker compose logs --tail=80 signaling sfu coturn
