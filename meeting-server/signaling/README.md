# Signaling Server (Phase 1 Skeleton)

## 运行

```bash
go run .
```

默认监听 `:8443`。

## 测试

```bash
go test ./...
```

## 当前实现范围

- 二进制帧协议（Magic + Version + Type + Length + Payload）
- Session 生命周期与并发读写模型
- 基础路由与状态校验
- 认证（内存用户 + 轻量 Token）
- 会议创建/加入/离开（内存存储）
- 会议内聊天广播（内存消息）

## 已知限制

- 当前 payload 已切换为 Protobuf 二进制（基于 `proto/signaling.proto` 生成的 `protocol/pb`）
- Redis/MySQL/SFU RPC 尚未接入（后续阶段接入）

