# Development Progress

> 最后更新：2026-04-22 | 当前收口：本机双实例 P2P 音视频稳定性

## Current Snapshot

- 本轮已按“双实例优先”修复客户端 P2P 音视频路径，未修改 protobuf、Go 信令或 SFU 传输协议。
- 音频侧已补齐 `RingBuffer::reset()`、synthetic audio 实际产帧、`AudioCapture::start()` 真实失败返回、音频 JitterBuffer 预缓冲/缺包等待，以及 `AudioPlayer` 按 `QAudioSink::bytesFree()` 节拍写入。
- 视频侧已调整显式摄像头设备的 Windows backend 顺序，强制 backend 仍尊重 `MEETING_CAMERA_CAPTURE_BACKEND`；Qt 摄像头错误会写入 `lastError`；real-device 状态推迟到首个有效帧；摄像头 3 秒无帧会中止 camera sending 并触发 smoke retry。
- 双实例脚本已支持 `-RequireAudio`，并在 `-RequireVideo -ExpectRealCamera` 且未显式传设备时默认匹配 host=`DroidCam Video`、guest=`e2eSoft iVCam`。
- 验证已通过：`meeting_client` Debug 构建、`test_jitter_buffer`、`audio_codec_smoke`、`test_camera_capture_smoke`(DroidCam Video/e2eSoft iVCam)、`test_screen_share_session_camera_smoke`(DroidCam Video/e2eSoft iVCam)、相关 `ctest` 子集、PowerShell 脚本语法检查。
- 双实例真实摄像头 e2e 已通过：`run_meeting_client_dual_ui_smoke.ps1 -RequireAudio -RequireVideo -ExpectRealCamera -RequireAvSync -HostCameraDevice "DroidCam Video" -GuestCameraDevice "e2eSoft iVCam" -CameraCaptureBackend dshow -TimeoutSeconds 60`。
- 注意：iVCam 在当前沙箱内 DirectShow 绑定仍失败，但沙箱外 ffmpeg、客户端摄像头 smoke 和双实例 e2e 均通过；真实设备验证需使用沙箱外权限运行。
- 完整 `ctest -C Debug --output-on-failure` 本轮运行超过 5 分钟被截断；`ctest -N` 显示当前 build 目录仍有若干未构建测试可执行文件，需单独整理完整测试目录后再跑全量。
- 当前阻塞：本机 DirectShow 能列出 `e2eSoft iVCam`，但 `ffmpeg -f dshow -i "video=e2eSoft iVCam"` 与客户端 smoke 均无法 BindToObject，表现为设备/驱动服务未就绪或被外部占用；待修复本机 iVCam 状态后复测双真实摄像头 e2e。

## Phase Notes

| Phase | 状态 | 本轮变化 |
|-------|------|----------|
| Phase 4 音频管线 | `partial -> improved` | 双实例卡顿风险降低：接收端增加预缓冲，播放端按设备可写空间节拍输出，synthetic audio 可作为真实测试源。 |
| Phase 5 视频管线 | `partial -> improved` | 摄像头成功语义改为首帧确认，显式双摄设备选择和无帧失败恢复路径已落地。 |
| Phase 6 SFU | `unchanged` | 本轮仍只保证本机 P2P；客户端改连 SFU 另开任务。 |

## Verification

```powershell
cmake --build . --target test_jitter_buffer audio_codec_smoke test_camera_capture_smoke test_screen_share_session_camera_smoke --config Debug
.\Debug\test_jitter_buffer.exe
.\Debug\audio_codec_smoke.exe
$env:MEETING_TEST_CAMERA_DEVICE='DroidCam Video'; $env:MEETING_CAMERA_CAPTURE_BACKEND='dshow'; .\Debug\test_camera_capture_smoke.exe
$env:MEETING_TEST_CAMERA_DEVICE='DroidCam Video'; $env:MEETING_CAMERA_CAPTURE_BACKEND='dshow'; .\Debug\test_screen_share_session_camera_smoke.exe
ctest -C Debug -R "test_jitter_buffer|audio_codec_smoke" --output-on-failure
```
