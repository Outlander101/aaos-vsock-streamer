# Overview
ScreenTransfer is a native Android userspace binary for low-latency H.264 streaming of an Android display over AF_VSOCK.    
It:    
1. Mirrors a physical Android display (e.g., Cluster / Display1)  
2. Encodes frames using MediaCodec (H.264 / AVC)  
3. Streams Annex-B formatted Access Units   
4. Uses a lightweight custom framing protocol (FRAM)   
5. Transmits over virtio-vsock to a remote VM (e.g., AGL) via Host broker.    
6. Supports reconnect without restarting encoder  

---

## Architecture
``` bash
SurfaceFlinger -> Virtual Display Mirror -> MediaCodec (H.264 Encoder) ->    
Annex-B Access Units -> FRAM framing protocol -> AF_VSOCK (CID:PORT) -> Host Broker
```
- The encoder remains alive across vsock reconnects. If the remote side restarts, ScreenTransfer reconnects automatically.

---

## Features
1. Low-latency H.264 streaming
2. Annex-B output (SPS/PPS + IDR)
3. SPS/PPS sent:
- On format change
- Before every keyframe
4. No B-frames (low decode latency)
5. Reconnect logic for vsock failures
6. Cluster display support (Display1 preferred)
7. Configurable resolution & bitrate
8. Clean EOS handling
9. Framing Protocol (FRAM)
10. Each Access Unit is sent as:
``` bash
20-byte header (little-endian)

[0..3]   'F','R','A','M'
[4..7]   payload length (uint32)
[8..15]  presentation timestamp (uint64, microseconds)
[16..19] flags (uint32)
```

### Flags:
| Flag | Meaning |
| 0x01 | Keyframe (IDR) |
| 0x04 | EOS |
| 0x08 |Codec config (SPS/PPS) |

- Payload is Annex-B H.264.

### Default Encoding Parameters
| Parameter	| Default |
| Codec	| H.264 (video/avc) |
| Resolution |	854x480 |
| Bitrate |	1.5 Mbps |
| GOP |	2s |
| B-frames | 0 |
| Profile |	Baseline-compatible |
| Frame rate |	60fps |


### Display Selection
By default:
- 1If multiple displays exist selects index 1 (Cluster)
- Else selects primary display
- Can be overriden using:
```bash
--display-id <physical_display_id>
```

---

## Build Instructions (AOSP)
This binary is intended to be built inside Android source tree.
1. Place Source
frameworks/av/cmds/screentransfer/
2. Ensure you have:
``` bash
screentransfer.cpp
screentransfer.h
VsockUtils.h
Android.bp
```
4. Build
``` bash
source build/envsetup.sh
lunch <target>
m screentransfer
Output:
out/target/product/<device>/system/bin/screentransfer
```

### Runtime Requirements
1. Kernel
``` bash
CONFIG_VSOCKETS
CONFIG_VIRTIO_VSOCKETS
```
2. Android
``` bash
MediaCodec H.264 encoder available
SurfaceFlinger running
Display active (Cluster must not be INVALID_LAYER_STACK)
Must listen on vsock CID:PORT
Must parse FRAM protocol
Must accept Annex-B H.264
Running ScreenTransfer
```

---

## Usage
1. Basic:
``` bash
adb shell screentransfer --vsock 2:22345
```
2. Example with resolution and bitrate:
``` bash
adb shell screentransfer \
    --size 640x480 \
    --bit-rate 1M \
    --vsock 2:22345
```
3. Command Line Options
| Option | Description |
| --size WIDTHxHEIGHT |	Set video resolution (Default 854x480) |
| --bit-rate RATE |	Bits/sec or '4M' format (Default 1.5Mbps) |
| --vsock CID:PORT |Target vsock endpoint (Default 2:22345) |
| --display-id ID |	Physical display ID (Default Instrument Cluster Display)| 
| --verbose	| Enable logging | 
| --help | Show usage |
