/*
 * screenmirror.h - Public interface and configuration structs
 *
 */

#ifndef SCREENMIRROR_H
#define SCREENMIRROR_H

#include <media/stagefright/foundation/AString.h>

#include <cstdint>
#include <optional>
#include <string>

namespace screenmirror {
using android::AString;

// Version information
constexpr uint32_t kVersionMajor = 1;
constexpr uint32_t kVersionMinor = 3;

// Bitrate constraints (bps)
constexpr uint32_t kMinBitRate = 100000u;   // 100 Kbps (safety floor)
constexpr uint32_t kMaxBitRate = 5000000u;  // 5 Mbps (safety ceiling)

// Video format
constexpr char kMimeTypeAvc[] = "video/avc";

// Frame flags (FRAM protocol)
constexpr uint32_t kFrameFlagKeyframe = 0x01u;  // IDR frame (I-frame)
constexpr uint32_t kFrameFlagEos = 0x04u;       // End-of-stream
constexpr uint32_t kFrameFlagConfig = 0x08u;    // SPS/PPS

// CSD heartbeat interval (seconds)
constexpr uint32_t kCsdHeartbeatIntervalSec = 2;

/**
 * Encoder configuration
 */
struct EncoderConfig {
  uint32_t width{854};        // Video width (pixels) 480p widescreen (16:9)
  uint32_t height{480};       // Video height (pixels)
  uint32_t bitrate{1500000};  // Target bitrate (bps)
  uint32_t maxBFrames{0};     // B-frames (0 for low latency)
  bool sizeSpecified{false};  // User overrode resolution
};

/**
 * VSOCK configuration
 */
struct VsockConfig {
  uint32_t port{22345};  // Listen port
  bool enabled{true};    // VSOCK enabled
};

/**
 * Runtime configuration
 */
struct RuntimeConfig {
  bool verbose{true};        // Verbose logging (always on by default)
  bool frameLogging{false};  // Per-frame logging
  bool rotate{false};  // Rotate display 90 degree (unused in current setup)
  bool persistentSurface{
      false};         // Use PersistentSurface (for multi-encoder setups)
  AString codecName;  // Force codec (empty = auto)
};

}  // namespace screenmirror

#endif  // SCREENMIRROR_H