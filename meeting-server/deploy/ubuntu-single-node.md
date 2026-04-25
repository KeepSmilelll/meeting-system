# Ubuntu Single-Node Deployment

Target: one Tencent Cloud Ubuntu host running signaling, SFU, MySQL, Redis, and coturn through Docker Compose.

## 1. Security Group

Open only these inbound rules on the cloud firewall:

| Protocol | Port | Purpose |
| --- | --- | --- |
| TCP | 22 | SSH |
| TCP | 8443 | Signaling |
| UDP | 10000 | SFU media |
| UDP | 3478 | TURN |
| TCP | 3478 | TURN fallback |
| UDP | 49152-49252 | TURN relay ports |

Do not expose MySQL or Redis to the public network.

## 2. Install Docker

Run on the Ubuntu server:

```bash
sudo apt update
sudo apt install -y ca-certificates curl gnupg
sudo install -m 0755 -d /etc/apt/keyrings
curl -fsSL https://download.docker.com/linux/ubuntu/gpg | sudo gpg --dearmor -o /etc/apt/keyrings/docker.gpg
sudo chmod a+r /etc/apt/keyrings/docker.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] https://download.docker.com/linux/ubuntu $(. /etc/os-release && echo "$VERSION_CODENAME") stable" | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

## 3. Upload Code

Put this repository on the server, for example:

```bash
sudo mkdir -p /opt/meeting
sudo chown "$USER:$USER" /opt/meeting
# rsync/scp/git clone the repository into /opt/meeting
cd /opt/meeting/meeting-server
```

## 4. Configure

```bash
cp .env.example .env
nano .env
```

Required edits:

- Keep `PUBLIC_IP=123.207.41.63` unless the cloud public IP changes.
- Replace `MYSQL_ROOT_PASSWORD`, `MYSQL_PASSWORD`, `JWT_SECRET`, and `TURN_SECRET`.
- Keep `TURN_MIN_PORT` and `TURN_MAX_PORT` aligned with the Tencent Cloud security group.
- If the host cannot reach Docker Hub, set `GOLANG_IMAGE`, `ALPINE_IMAGE`, `UBUNTU_IMAGE`, `MYSQL_IMAGE`, `REDIS_IMAGE`, and `COTURN_IMAGE` to a reachable registry mirror.

Example mirror override:

```bash
cat >> .env <<'EOF'
GOLANG_IMAGE=docker.m.daocloud.io/library/golang:1.22-alpine
ALPINE_IMAGE=docker.m.daocloud.io/library/alpine:3.20
UBUNTU_IMAGE=docker.m.daocloud.io/library/ubuntu:24.04
MYSQL_IMAGE=docker.m.daocloud.io/library/mysql:8.4
REDIS_IMAGE=docker.m.daocloud.io/library/redis:7.4-alpine
COTURN_IMAGE=docker.m.daocloud.io/coturn/coturn:latest
EOF
```

## 5. Start

```bash
docker compose up -d --build
docker compose ps
docker compose logs -f signaling sfu coturn
```

Expected state:

- `mysql` and `redis` are healthy.
- `sfu` logs `meeting_sfu started`.
- `signaling` listens on TCP `8443`.
- `coturn` listens on UDP/TCP `3478` and allocates relay ports in `49152-49252`.

## 6. Client Settings

For a client connecting from the public Internet:

```text
host: 123.207.41.63
port: 8443
```

The signaling join response advertises:

```text
sfu: 123.207.41.63:10000
turn: 123.207.41.63:3478?transport=udp
```

Use relay-only ICE only for firewall validation. Normal Internet validation should use the default ICE policy.

## 7. Operations

```bash
docker compose ps
docker compose logs --tail=200 signaling
docker compose logs --tail=200 sfu
docker compose logs --tail=200 coturn
docker compose restart signaling sfu coturn
```

Stop the stack:

```bash
docker compose down
```

Remove database data only when intentionally resetting the environment:

```bash
docker compose down -v
```

## Current Limits

- This is a single-node deployment, not high availability.
- TLS/Nginx is not enabled yet; TCP signaling is exposed directly on `8443`.
- TURN is configured for shared-secret credentials and UDP relay validation.
