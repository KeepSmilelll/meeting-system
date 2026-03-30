# Ubuntu 24.04 LTS 服务器部署指南

> **硬件**：i5-9300H (4C8T) · 16 GiB RAM
> **系统**：Ubuntu 24.04 LTS (Noble Numbat)
> **环境**：MariaDB / MySQL 8 · Docker

---

## 1. 组件清单总览

```
┌──────────────────────────────────────────────────────────────┐
│                    部署状态一览                                │
│                                                               │
│  ✅ 已就绪          ⬜ 待安装 (apt)          📦 Docker 部署   │
│                                                               │
│  ✅ MariaDB/MySQL   ⬜ Redis-Server          📦 coturn        │
│  ✅ Docker          ⬜ Nginx                                  │
│                     ⬜ build-essential (GCC/G++)              │
│                     ⬜ CMake + Ninja-build                    │
│                     ⬜ libboost-all-dev                       │
│                     ⬜ protobuf-compiler + libprotobuf-dev    │
│                     ⬜ libssl-dev (OpenSSL)                   │
│                     ⬜ libhiredis-dev                         │
│                     ⬜ ffmpeg + libavcodec-dev                │
│                     ⬜ libgtest-dev                           │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 安装命令

### 2.1 基础环境与编译工具链

Ubuntu 24.04 LTS 默认提供 GCC 13 和 CMake 3.28，完美支持 C++17/20。

```bash
# 更新软件源
sudo apt update && sudo apt upgrade -y

# C++ 编译器与构建系统
sudo apt install -y build-essential cmake ninja-build gdb clang

# 验证版本
g++ --version      # 应为 13.x
cmake --version    # 应为 3.28.x
```

### 2.2 核心依赖库

```bash
# Boost (用于 Asio 网络库等)
sudo apt install -y libboost-all-dev

# Protobuf (信令序列化)
sudo apt install -y protobuf-compiler libprotobuf-dev

# OpenSSL
sudo apt install -y libssl-dev

# FFmpeg 运行时与开发库
sudo apt install -y ffmpeg libavcodec-dev libavformat-dev libavutil-dev libswresample-dev

# Redis C++ 客户端底层库
sudo apt install -y libhiredis-dev

# GTest 单元测试
sudo apt install -y libgtest-dev
```

### 2.3 Redis 服务端

```bash
# 安装 Redis
sudo apt install -y redis-server

# 启动并设为开机自启
sudo systemctl enable --now redis-server

# 验证
redis-cli ping    # 应返回 PONG
```

### 2.4 Nginx

```bash
sudo apt install -y nginx
sudo systemctl enable --now nginx
```

### 2.5 coturn (STUN/TURN — 推荐 Docker 部署)

虽然 apt 也有 coturn，但在 Docker 中部署网络隔离更干净，且更容易升级。

```bash
# 拉取镜像
sudo docker pull coturn/coturn:latest

# 创建配置目录
mkdir -p ~/meeting-server/turn

# 启动 coturn 容器 (使用 host 网络模式以获得最佳 UDP 转发性能)
sudo docker run -d --name coturn \
  --network=host \
  --restart=always \
  -v ~/meeting-server/turn/turnserver.conf:/etc/turnserver.conf \
  coturn/coturn:latest
```

---

## 3. 服务配置

### 3.1 Redis 配置 (`/etc/redis/redis.conf`)

对 16GB 内存服务器的配置建议：

```conf
bind 127.0.0.1 ::1
port 6379

# 限制最大内存为 2GB，策略为 LRU
maxmemory 2gb
maxmemory-policy allkeys-lru

# 持久化设置 (AOF 模式)
appendonly yes
appendfsync everysec

# 高频事件轮询
hz 100
tcp-keepalive 300
```
修改后重启：`sudo systemctl restart redis-server`

### 3.2 MariaDB/MySQL 调优 (`/etc/mysql/mariadb.conf.d/50-server.cnf`)

```ini
[mysqld]
character-set-server = utf8mb4
collation-server = utf8mb4_unicode_ci

max_connections = 200

# 16GB 内存分配 4GB 给数据库缓冲池
innodb_buffer_pool_size = 4G
innodb_log_file_size = 256M
innodb_flush_log_at_trx_commit = 2
innodb_flush_method = O_DIRECT

slow_query_log = 1
long_query_time = 0.1
```
修改后重启：`sudo systemctl restart mariadb` 或 `sudo systemctl restart mysql`

### 3.3 Nginx 配置 (`/etc/nginx/nginx.conf`)

为支持 TCP 长连接代理和 SSL 终结，需要在最外层添加 `stream` 块：

```nginx
events {
    worker_connections 2048;
    use epoll;
}

# TCP/TLS 信令代理
stream {
    upstream signaling_backend {
        server 127.0.0.1:8443;
    }

    server {
        listen 443 ssl;
        proxy_pass signaling_backend;

        ssl_certificate     /etc/ssl/certs/meeting_selfsigned.crt;
        ssl_certificate_key /etc/ssl/private/meeting_selfsigned.key;
        ssl_protocols       TLSv1.2 TLSv1.3;

        proxy_timeout 300s;
        proxy_connect_timeout 10s;
    }
}
```

**生成开发自签证书：**
```bash
sudo openssl req -x509 -nodes -days 365 -newkey rsa:2048 \
  -keyout /etc/ssl/private/meeting_selfsigned.key \
  -out /etc/ssl/certs/meeting_selfsigned.crt \
  -subj "/CN=meeting.local"
```

### 3.4 coturn 配置 (`~/meeting-server/turn/turnserver.conf`)

```conf
# 基础端口
listening-port=3478
tls-listening-port=5349

# 填入服务器的公网和内网IP
listening-ip=0.0.0.0
relay-ip=<你的内网IP>
external-ip=<你的公网IP>/<你的内网IP>

# 限制中继端口范围 (防止占满端口)
min-port=49152
max-port=50175

# 认证相关
realm=meeting.local
lt-cred-mech
user=devuser:devpassword

fingerprint
no-multicast-peers
```

---

## 4. Ubuntu 系统内核级调优

由于经常处理高并发长连接和 UDP 音视频流，需要调整 Linux 内核参数。

创建配置文件：`sudo nano /etc/sysctl.d/99-meeting-server.conf`

```ini
# --- UDP 缓冲区优化 (SFU 必备) ---
net.core.rmem_max = 26214400          # 25MB 接收缓冲
net.core.wmem_max = 26214400          # 25MB 发送缓冲
net.core.rmem_default = 1048576
net.core.wmem_default = 1048576

# --- TCP 连接优化 (信令服务器必备) ---
net.ipv4.tcp_max_tw_buckets = 200000
net.ipv4.tcp_tw_reuse = 1
net.ipv4.tcp_keepalive_time = 300
net.ipv4.tcp_keepalive_intvl = 30
net.ipv4.tcp_keepalive_probes = 3
net.ipv4.tcp_fin_timeout = 15
net.ipv4.tcp_max_syn_backlog = 8192

# --- 文件描述符 ---
fs.file-max = 655350
```

应用配置：`sudo sysctl -p /etc/sysctl.d/99-meeting-server.conf`

**用户级文件限制：**
修改 `sudo nano /etc/security/limits.conf`，追加：
```
* soft nofile 65535
* hard nofile 65535
```

---

## 5. 防火墙配置 (UFW)

Ubuntu 默认使用 UFW 简化防火墙管理：

```bash
# 开启 SSH 防掉线
sudo ufw allow ssh

# 允许 Nginx TLS 端口
sudo ufw allow 443/tcp

# 允许 coturn STUN/TURN
sudo ufw allow 3478/tcp
sudo ufw allow 3478/udp
sudo ufw allow 5349/tcp
sudo ufw allow 49152:50175/udp

# 允许 SFU RTP 媒体流范围
sudo ufw allow 10000:10100/udp

# 启用防火墙
sudo ufw enable
```
*(注意：不要把 Redis 的 6379 和 MariaDB 的 3306 开放给公网)*

---

## 6. 日常运维脚本

建立一个启动/停止聚合脚本，方便开发：

**`start_all.sh`**
```bash
#!/bin/bash
sudo systemctl start mariadb redis-server nginx
sudo docker start coturn
echo "基础服务已启动完毕！"
echo "请手动执行 cmake 编译的信令服务器和 SFU 服务器。"
```

**`stop_all.sh`**
```bash
#!/bin/bash
sudo systemctl stop nginx redis-server
sudo docker stop coturn
echo "基础服务已停止！(MariaDB 保留运行状态)"
```
