/*
 * screenmirror.cpp - H.264 streaming over VSOCK
 *
 */

#include "screenmirror.h"

#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <vector>

#include "VsockUtils.h"

#define LOG_TAG "ScreenMirror"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#include <binder/IPCThreadState.h>
#include <gui/ISurfaceComposer.h>
#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <media/MediaCodecBuffer.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaCodecConstants.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/PersistentSurface.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <mediadrm/ICrypto.h>
#include <ui/DisplayMode.h>
#include <ui/DisplayState.h>
#include <utils/Errors.h>
#include <utils/Log.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

using android::ABuffer;
using android::ALooper;
using android::AMessage;
// using android::AString;
using android::IBinder;
using android::IGraphicBufferProducer;
using android::INVALID_OPERATION;
using android::MediaCodec;
using android::MediaCodecBuffer;
using android::NO_ERROR;
using android::PersistentSurface;
using android::PhysicalDisplayId;
using android::ProcessState;
using android::Rect;
using android::sp;
using android::status_t;
using android::String8;
using android::SurfaceComposerClient;
using android::SurfaceControl;
using android::UNKNOWN_ERROR;
using android::Vector;
using android::ui::DisplayMode;

namespace ui = android::ui;

using namespace screenmirror;

// Global configuration (initialized from command-line arguments)
EncoderConfig gEncoderConfig;
VsockConfig gVsockConfig;
RuntimeConfig gRuntimeConfig;

// Signal handling
static volatile bool gStopRequested = false;
static struct sigaction gOrigSigactionINT;
static struct sigaction gOrigSigactionHUP;

// Connection lifecycle management
// gConnectionCount tracks the number of vsock connections established:
//   0 = Initial state (no connection yet, encoder not started)
//   1 = First connection (send natural CSD+IDR from encoder)
//   2+ = Reconnection (send cached CSD+IDR for fast-start)
static uint32_t gConnectionCount = 0;
static std::mutex gConnectionMutex;

static std::optional<PhysicalDisplayId> gPhysicalDisplayId;

// CSD Heartbeat Time
static int64_t gLastCsdHeartbeatTimeUs = 0;
// Encoder pause state
// When vsock disconnects, encoder pauses (stops dequeuing frames) to save CPU.
// Virtual display mirror continues (can't pause SurfaceFlinger), but encoder is
// idle.
static bool gEncoderPaused = false;
static std::mutex gEncoderMutex;

// Cached data for fast-start recovery
// These are populated during normal streaming and sent immediately on
// reconnect.
static sp<ABuffer>
    gCachedCSDAnnexB;  // SPS + PPS (set once at INFO_FORMAT_CHANGED)
static sp<ABuffer> gCachedIDRFrame;  // Latest IDR frame (updated on every IDR)
static int64_t gCachedIDRPtsUs = 0;  // PTS of cached IDR
static std::mutex gCacheMutex;       // Protects all cached data

/**
 * signalCatcher - Handle SIGINT and SIGHUP gracefully
 *
 * Sets gStopRequested flag to break encoder loop, then restores original
 * handlers so a second Ctrl-C force-kills the process.
 *
 * Arguments:
 *   @param signum Signal number (SIGINT=2, SIGHUP=1)
 * Return: None
 */
static void signalCatcher(int signum) {
  gStopRequested = true;
  switch (signum) {
    case SIGINT:
    case SIGHUP:
      sigaction(SIGINT, &gOrigSigactionINT, NULL);
      sigaction(SIGHUP, &gOrigSigactionHUP, NULL);
      break;
    default:
      abort();
  }
}

/**
 * configureSignals - Install signal handlers for clean shutdown
 *
 * Registers signalCatcher() for SIGINT (Ctrl-C) and SIGHUP (terminal close).
 * Ignores SIGPIPE to prevent crash when broker closes vsock.
 *
 * Arguments: None
 * Return: NO_ERROR on success, -errno on failure
 */
static status_t configureSignals() {
  struct sigaction act;
  memset(&act, 0, sizeof(act));
  act.sa_handler = signalCatcher;

  if (sigaction(SIGINT, &act, &gOrigSigactionINT) != 0) {
    status_t err = -errno;
    ALOGE("Unable to configure SIGINT handler: %s\n", strerror(errno));
    return err;
  }
  if (sigaction(SIGHUP, &act, &gOrigSigactionHUP) != 0) {
    status_t err = -errno;
    ALOGE("Unable to configure SIGHUP handler: %s\n", strerror(errno));
    return err;
  }

  signal(SIGPIPE, SIG_IGN);
  return NO_ERROR;
}

/**
 * getNalTypeName - Extract NAL unit type from Annex-B payload
 *
 * Skips start code (0x000001 or 0x00000001) and reads NAL header byte.
 *
 * Arguments:
 *   @param data Pointer to Annex-B NAL data
 *   @param size Size of data in bytes
 * Return: Human-readable NAL type (e.g., "IDR-SLICE", "SPS")
 */
static const char* getNalTypeName(const uint8_t* data, size_t size) {
  if (!data || size < 5) return "UNKNOWN";

  size_t nalStart = 0;
  if (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
      data[3] == 1) {
    nalStart = 4;
  } else if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
    nalStart = 3;
  } else {
    return "NO_START_CODE";
  }

  if (nalStart >= size) return "TRUNCATED";

  uint8_t nalType = data[nalStart] & 0x1F;
  switch (nalType) {
    case 1:
      return "P-SLICE";
    case 2:
      return "DPA-SLICE";
    case 3:
      return "DPB-SLICE";
    case 4:
      return "DPC-SLICE";
    case 5:
      return "IDR-SLICE";
    case 6:
      return "SEI";
    case 7:
      return "SPS";
    case 8:
      return "PPS";
    case 9:
      return "AUD";
    case 10:
      return "END-SEQ";
    case 11:
      return "END-STREAM";
    case 12:
      return "FILLER";
    default:
      return "UNKNOWN";
  }
}

/**
 * countNalUnits - Count NAL units in Annex-B buffer
 *
 * Scans for start codes to count NALs. Used for CSD validation (expect 2:
 * SPS+PPS).
 *
 * Arguments:
 *   @param data Pointer to Annex-B buffer
 *   @param size Buffer size in bytes
 * Return: Number of NAL units found
 */
static int countNalUnits(const uint8_t* data, size_t size) {
  if (!data || size < 4) return 0;

  int count = 0;
  size_t i = 0;
  while (i + 3 < size) {
    if ((i + 4 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 &&
         data[i + 3] == 1) ||
        (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)) {
      count++;
      i += (data[i + 2] == 1) ? 3 : 4;
    } else {
      i++;
    }
  }
  return count;
}

/**
 * prepareEncoder - Configure and start H.264 hardware encoder
 *
 * Creates MediaCodec encoder with low-latency settings:
 *   - Baseline profile (fastest decode)
 *   - 0 B-frames (no buffering)
 *   - 2-second GOP (IDR every 2s)
 *   - Surface input (zero-copy from SurfaceFlinger)
 *
 * Encoder is created once and kept alive across vsock reconnects.
 *
 * Arguments:
 *   @param displayFps Native refresh rate (e.g., 60.0)
 *   @param pCodec Output: Created MediaCodec instance
 *   @param pBufferProducer Output: Surface for SurfaceFlinger
 * Return: NO_ERROR on success, error code on failure
 */
static status_t prepareEncoder(float displayFps, sp<MediaCodec>* pCodec,
                               sp<IGraphicBufferProducer>* pBufferProducer) {
  if (gRuntimeConfig.verbose) {
    printf(" Encoder Configuration\n");
    printf(" Resolution:   %ux%u\n", gEncoderConfig.width,
           gEncoderConfig.height);
    printf(" Bitrate:      %.2f Mbps\n", gEncoderConfig.bitrate / 1000000.0);
    printf(" Frame Rate:   %.2f fps\n", displayFps);
    printf(" GOP Interval: 2 seconds (IDR every %.0f frames)\n",
           displayFps * 2.0);
    printf(" CSD Heartbeat: Every %us (during streaming)\n",
           kCsdHeartbeatIntervalSec);
    printf(" Fast-Start:   Cached CSD + IDR on reconnect\n");
    fflush(stdout);
  }

  sp<AMessage> format = new AMessage;
  format->setInt32(KEY_WIDTH, gEncoderConfig.width);
  format->setInt32(KEY_HEIGHT, gEncoderConfig.height);
  format->setString(KEY_MIME, kMimeTypeAvc);
  format->setInt32(KEY_COLOR_FORMAT, OMX_COLOR_FormatAndroidOpaque);
  format->setInt32(KEY_BIT_RATE, gEncoderConfig.bitrate);
  format->setFloat(KEY_FRAME_RATE, displayFps);
  format->setInt32(KEY_I_FRAME_INTERVAL, 2);
  format->setInt32(KEY_MAX_B_FRAMES, 0);

  sp<ALooper> looper = new ALooper;
  looper->setName("screenmirror_looper");
  looper->start();

  sp<MediaCodec> codec;
  if (gRuntimeConfig.codecName.empty()) {
    codec = MediaCodec::CreateByType(looper, kMimeTypeAvc, true);
    if (codec == NULL) {
      ALOGE("ERROR: unable to create H.264 codec\n");
      return UNKNOWN_ERROR;
    }
  } else {
    codec = MediaCodec::CreateByComponentName(looper, gRuntimeConfig.codecName);
    if (codec == NULL) {
      ALOGE("ERROR: unable to create codec '%s'\n",
            gRuntimeConfig.codecName.c_str());
      return UNKNOWN_ERROR;
    }
  }

  status_t err =
      codec->configure(format, NULL, NULL, MediaCodec::CONFIGURE_FLAG_ENCODE);
  if (err != NO_ERROR) {
    ALOGE("ERROR: codec configure failed (err=%d)\n", err);
    codec->release();
    return err;
  }

  sp<IGraphicBufferProducer> bufferProducer;
  if (gRuntimeConfig.persistentSurface) {
    sp<PersistentSurface> surface = MediaCodec::CreatePersistentInputSurface();
    bufferProducer = surface->getBufferProducer();
    err = codec->setInputSurface(surface);
  } else {
    err = codec->createInputSurface(&bufferProducer);
  }
  if (err != NO_ERROR) {
    ALOGE("ERROR: unable to create input surface (err=%d)\n", err);
    codec->release();
    return err;
  }

  err = codec->start();
  if (err != NO_ERROR) {
    ALOGE("ERROR: codec start failed (err=%d)\n", err);
    codec->release();
    return err;
  }

  ALOGI("Codec started successfully");
  *pCodec = codec;
  *pBufferProducer = bufferProducer;
  return NO_ERROR;
}

/**
 * setDisplayProjection - Configure virtual display crop and rotation
 *
 * Calculates aspect-ratio-preserving letterboxing.
 *
 * Arguments:
 *   @param t SurfaceFlinger transaction
 *   @param dpy Virtual display handle
 *   @param displayState Physical display state
 * Return: NO_ERROR on success
 */
static status_t setDisplayProjection(SurfaceComposerClient::Transaction& t,
                                     const sp<IBinder>& dpy,
                                     const ui::DisplayState& displayState) {
  Rect layerStackRect(displayState.layerStackSpaceRect);
  float displayAspect = layerStackRect.getHeight() /
                        static_cast<float>(layerStackRect.getWidth());

  uint32_t videoWidth =
      gRuntimeConfig.rotate ? gEncoderConfig.height : gEncoderConfig.width;
  uint32_t videoHeight =
      gRuntimeConfig.rotate ? gEncoderConfig.width : gEncoderConfig.height;

  uint32_t outWidth, outHeight;
  if (videoHeight > (uint32_t)(videoWidth * displayAspect)) {
    outWidth = videoWidth;
    outHeight = (uint32_t)(videoWidth * displayAspect);
  } else {
    outHeight = videoHeight;
    outWidth = (uint32_t)(videoHeight / displayAspect);
  }

  uint32_t offX = (videoWidth - outWidth) / 2;
  uint32_t offY = (videoHeight - outHeight) / 2;
  Rect displayRect(offX, offY, offX + outWidth, offY + outHeight);

  t.setDisplayProjection(
      dpy, gRuntimeConfig.rotate ? ui::ROTATION_90 : ui::ROTATION_0,
      layerStackRect, displayRect);
  return NO_ERROR;
}

/**
 * getPhysicalDisplayId - Get display ID to mirror
 *
 * Prefers cluster display (index 1) if available, else primary (index 0).
 *
 * Arguments:
 *   @param outDisplayId Output: Physical display ID
 * Return: NO_ERROR on success, INVALID_OPERATION if no displays
 */
static status_t getPhysicalDisplayId(PhysicalDisplayId& outDisplayId) {
  if (gPhysicalDisplayId) {
    outDisplayId = *gPhysicalDisplayId;
    return NO_ERROR;
  }
  const std::vector<PhysicalDisplayId> ids =
      SurfaceComposerClient::getPhysicalDisplayIds();
  if (ids.empty()) return INVALID_OPERATION;

  outDisplayId = (ids.size() > 1) ? ids[1] : ids[0];
  return NO_ERROR;
}

/**
 * prepareVirtualDisplay - Create virtual display and mirror physical display
 *
 * Arguments:
 *   @param displayState Physical display state
 *   @param bufferProducer Encoder input surface
 *   @param pDisplayHandle Output: Virtual display handle
 *   @param mirrorRoot Output: Mirror SurfaceControl
 * Return: NO_ERROR on success
 */
static status_t prepareVirtualDisplay(
    const ui::DisplayState& displayState,
    const sp<IGraphicBufferProducer>& bufferProducer,
    sp<IBinder>* pDisplayHandle, sp<SurfaceControl>* mirrorRoot) {
  sp<IBinder> dpy =
      SurfaceComposerClient::createDisplay(String8("ScreenMirror"), false);

  SurfaceComposerClient::Transaction t;
  t.setDisplaySurface(dpy, bufferProducer);
  setDisplayProjection(t, dpy, displayState);

  ui::LayerStack layerStack = ui::LayerStack::fromValue(std::rand());
  t.setDisplayLayerStack(dpy, layerStack);

  PhysicalDisplayId displayId;
  status_t err = getPhysicalDisplayId(displayId);
  if (err != NO_ERROR) return err;

  *mirrorRoot = SurfaceComposerClient::getDefault()->mirrorDisplay(displayId);
  if (*mirrorRoot == nullptr) {
    ALOGE("Failed to create mirror");
    return UNKNOWN_ERROR;
  }

  t.setLayerStack(*mirrorRoot, layerStack);
  t.apply();
  *pDisplayHandle = dpy;
  return NO_ERROR;
}

/**
 * updateDisplayProjection - Refresh projection on orientation change
 *
 * Arguments:
 *   @param virtualDpy Virtual display handle
 *   @param displayState Current display state (updated if changed)
 * Return: None
 */
static void updateDisplayProjection(const sp<IBinder>& virtualDpy,
                                    ui::DisplayState& displayState) {
  PhysicalDisplayId displayId;
  if (getPhysicalDisplayId(displayId) != NO_ERROR) return;

  sp<IBinder> displayToken =
      SurfaceComposerClient::getPhysicalDisplayToken(displayId);
  if (!displayToken) return;

  ui::DisplayState current;
  if (SurfaceComposerClient::getDisplayState(displayToken, &current) !=
      NO_ERROR)
    return;

  if (current.orientation != displayState.orientation ||
      current.layerStackSpaceRect != displayState.layerStackSpaceRect) {
    displayState = current;
    ALOGI("Display changed: orientation=%s size=(%d,%d)",
          toCString(displayState.orientation),
          displayState.layerStackSpaceRect.getWidth(),
          displayState.layerStackSpaceRect.getHeight());
    SurfaceComposerClient::Transaction t;
    setDisplayProjection(t, virtualDpy, current);
    t.apply();
  }
}

/**
 * writeValueLE - Write integer in little-endian byte order
 *
 * Arguments:
 *   @param value Integer to write
 *   @param buffer Output buffer (must have sizeof(T) bytes)
 * Return: None
 */
template <typename T>
static void writeValueLE(T value, uint8_t* buffer) {
  std::remove_const_t<T> temp = value;
  for (int i = 0; i < (int)sizeof(T); ++i) {
    buffer[i] = static_cast<uint8_t>(temp & 0xff);
    temp >>= 8;
  }
}

/**
 * sendFramedAu - Send FRAM-wrapped H.264 access unit over vsock
 *
 * Frame format (20-byte header + payload):
 *   [0..3]   Magic: 'F','R','A','M'
 *   [4..7]   Payload length (uint32 LE)
 *   [8..15]  PTS in microseconds (uint64 LE)
 *   [16..19] Flags (uint32 LE): KEYFRAME | EOS | CONFIG
 *   [20..]   Payload (H.264 Annex-B NAL units)
 *
 * Arguments:
 *   @param fd VSOCK file descriptor
 *   @param payload Annex-B NAL data
 *   @param payloadLen Payload size in bytes
 *   @param ptsUs Presentation timestamp in microseconds
 *   @param flags Frame flags
 * Return: true on success, false on write error
 */
static bool sendFramedAu(int fd, const uint8_t* payload, uint32_t payloadLen,
                         uint64_t ptsUs, uint32_t flags) {
  uint8_t hdr[20];
  hdr[0] = 'F';
  hdr[1] = 'R';
  hdr[2] = 'A';
  hdr[3] = 'M';
  writeValueLE<uint32_t>(payloadLen, hdr + 4);
  writeValueLE<uint64_t>(ptsUs, hdr + 8);
  writeValueLE<uint32_t>(flags, hdr + 16);

  if (!VsockUtils::writeFully(fd, hdr, sizeof(hdr))) {
    ALOGE("Failed to send frame header (fd=%d errno=%s)", fd, strerror(errno));
    return false;
  }
  if (payloadLen > 0 && !VsockUtils::writeFully(fd, payload, payloadLen)) {
    ALOGE("Failed to send frame payload (len=%u errno=%s)", payloadLen,
          strerror(errno));
    return false;
  }
  return true;
}

/**
 * hasAnnexBStartCode - Check if buffer starts with Annex-B start code
 *
 * Arguments:
 *   @param buf Buffer to check
 *   @param size Buffer size
 * Return: true if starts with 0x000001 or 0x00000001
 */
static bool hasAnnexBStartCode(const uint8_t* buf, size_t size) {
  if (size >= 4 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x00 &&
      buf[3] == 0x01)
    return true;
  if (size >= 3 && buf[0] == 0x00 && buf[1] == 0x00 && buf[2] == 0x01)
    return true;
  return false;
}

/**
 * avccToAnnexB - Convert AVCC (length-prefixed) to Annex-B
 * (start-code-prefixed)
 *
 * AVCC format: [4-byte length][NAL][4-byte length][NAL]...
 * Annex-B format: [0x00000001][NAL][0x00000001][NAL]...
 *
 * Arguments:
 *   @param data AVCC buffer
 *   @param size Buffer size
 * Return: Annex-B buffer (empty on error)
 */
static std::vector<uint8_t> avccToAnnexB(const uint8_t* data, size_t size) {
  if (!data || size == 0) return {};
  static const uint8_t kSC[4] = {0, 0, 0, 1};
  std::vector<uint8_t> out;
  out.reserve(size + 32);
  const uint8_t* src = data;
  const uint8_t* end = data + size;

  while (src + 4 <= end) {
    uint32_t nalLen = (src[0] << 24) | (src[1] << 16) | (src[2] << 8) | src[3];
    src += 4;
    if (src + nalLen > end) {
      ALOGE("avccToAnnexB: invalid NAL length %u", nalLen);
      out.clear();
      return out;
    }
    out.insert(out.end(), kSC, kSC + 4);
    out.insert(out.end(), src, src + nalLen);
    src += nalLen;
  }
  return out;
}

/**
 * sendFastStartPacket - Send cached CSD + IDR for instant recovery
 *
 * On reconnection (gConnectionCount > 1), immediately send:
 *   1. Cached CSD (SPS + PPS)
 *   2. Cached IDR (latest keyframe)
 *
 * This allows AGL decoder to initialize instantly (<50ms) instead of
 * waiting up to 2 seconds for the next natural IDR.
 *
 * Arguments:
 *   @param vsockFd Connected vsock file descriptor
 * Return: true on success, false on send failure
 */
static bool sendFastStartPacket(int vsockFd) {
  std::lock_guard<std::mutex> lock(gCacheMutex);

  if (!gCachedCSDAnnexB || gCachedCSDAnnexB->size() == 0) {
    ALOGW("Fast-start: No cached CSD available");
    return false;
  }

  if (!gCachedIDRFrame || gCachedIDRFrame->size() == 0) {
    ALOGW("Fast-start: No cached IDR available");
    return false;
  }

  ALOGI("Fast-Start Recovery (Connection #%u)", gConnectionCount);
  ALOGI("Sending cached CSD + IDR for instant decoder init");
  ALOGI("CSD size: %zu bytes", gCachedCSDAnnexB->size());
  ALOGI("IDR size: %zu bytes (PTS: %" PRId64 ")", gCachedIDRFrame->size(),
        gCachedIDRPtsUs);

  // Send CSD first
  int64_t freshCSDPtsUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
  if (!sendFramedAu(vsockFd, gCachedCSDAnnexB->data(),
                    (uint32_t)gCachedCSDAnnexB->size(), freshCSDPtsUs,
                    kFrameFlagConfig)) {  // current Time
    ALOGE("Failed to send cached CSD");
    return false;
  }

  // Send cached IDR
  int64_t freshIDRPtsUs = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
  if (!sendFramedAu(vsockFd, gCachedIDRFrame->data(),
                    (uint32_t)gCachedIDRFrame->size(), freshIDRPtsUs,
                    kFrameFlagKeyframe)) {  // Current Time
    ALOGE("Failed to send cached IDR");
    return false;
  }

  ALOGI("Fast-start packet sent successfully");
  ALOGI("AGL decoder should initialize within 50ms");
  return true;
}

/**
 * cacheLatestIDR - Store most recent IDR frame for fast-start
 *
 * Called every time an IDR frame is encoded. Stores the frame data
 * so it can be resent immediately on reconnection.
 *
 * Arguments:
 *   @param data IDR frame data (Annex-B format)
 *   @param size Frame size in bytes
 *   @param ptsUs Frame PTS in microseconds
 * Return: None
 */
static void cacheLatestIDR(const uint8_t* data, size_t size, int64_t ptsUs) {
  if (!data || size == 0) return;

  std::lock_guard<std::mutex> lock(gCacheMutex);

  gCachedIDRFrame = new ABuffer(size);
  memcpy(gCachedIDRFrame->data(), data, size);
  gCachedIDRFrame->setRange(0, size);
  gCachedIDRPtsUs = ptsUs;

  if (gRuntimeConfig.frameLogging) {
    ALOGD("Cached IDR: %zu bytes, PTS: %" PRId64, size, ptsUs);
  }
}

/**
 * pauseEncoder - Pause encoder when vsock disconnects
 *
 * Sets gEncoderPaused flag to stop dequeuing frames.
 * Virtual display mirror continues (can't pause SurfaceFlinger).
 * Saves CPU/power during AGL reboot.
 *
 * Arguments: None
 * Return: None
 */
static void pauseEncoder() {
  std::lock_guard<std::mutex> lock(gEncoderMutex);
  if (!gEncoderPaused) {
    gEncoderPaused = true;
    ALOGI("Encoder PAUSED (vsock disconnected)");
  }
}

/**
 * resumeEncoder - Resume encoder when vsock reconnects
 *
 * Clears gEncoderPaused flag to restart dequeuing frames.
 * Increments gConnectionCount to track reconnections.
 *
 * Arguments: None
 * Return: None
 */
static void resumeEncoder() {
  std::lock_guard<std::mutex> lock(gEncoderMutex);
  if (gEncoderPaused) {
    gEncoderPaused = false;
    ALOGI("Encoder Resumed (vsock reconnected)");
  }

  std::lock_guard<std::mutex> connLock(gConnectionMutex);
  gConnectionCount++;
  ALOGI("Connection count: %u", gConnectionCount);
}

/**
 * isEncoderPaused - Check if encoder is paused
 *
 * Arguments: None
 * Return: true if paused, false if running
 */
static bool isEncoderPaused() {
  std::lock_guard<std::mutex> lock(gEncoderMutex);
  return gEncoderPaused;
}

/**
 * runEncoder - Main encoder loop with pause/resume and fast-start
 *
 * FLOW:
 *   1. On connection (gConnectionCount == 1):
 *      - Wait for INFO_FORMAT_CHANGED → cache CSD
 *      - Send natural CSD + IDR from encoder
 *
 *   2. During streaming (gConnectionCount >= 1):
 *      - Dequeue frames from encoder
 *      - Cache every IDR for fast-start
 *      - Send CSD heartbeat every 2s
 *      - Prepend CSD before every IDR (defense-in-depth)
 *
 *   3. On disconnection:
 *      - Detect vsock write failure
 *      - Pause encoder (stop dequeuing)
 *      - Return to allow reconnection
 *
 *   4. On reconnection (gConnectionCount > 1):
 *      - Resume encoder
 *      - Send cached CSD + cached IDR (fast-start!)
 *      - Resume normal streaming
 *
 * Arguments:
 *   @param codec Encoder instance
 *   @param vsockFd Connected vsock file descriptor
 *   @param verbose Enable detailed logging
 *   @param virtualDpy Virtual display handle
 *   @param displayState Current display state
 * Return: NO_ERROR on clean exit, UNKNOWN_ERROR on vsock failure
 */
static status_t runEncoder(const sp<MediaCodec>& codec, int vsockFd,
                           bool verbose, const sp<IBinder>& virtualDpy,
                           ui::DisplayState displayState) {
  Vector<sp<MediaCodecBuffer>> buffers;
  status_t err = codec->getOutputBuffers(&buffers);
  if (err != android::OK) {
    ALOGW("Initial getOutputBuffers failed (err=%d), will refresh", err);
    buffers.clear();
  }

  uint32_t frameCount = 0;
  uint32_t idrCount = 0;
  uint32_t pframeCount = 0;
  uint32_t csdSentCount = 0;
  uint32_t csdHeartbeatCount = 0;
  uint32_t timeoutCount = 0;
  int64_t startTimeNs = systemTime(CLOCK_MONOTONIC);

  // Resume encoder on entry
  resumeEncoder();

  // Reset heartbeat timer
  gLastCsdHeartbeatTimeUs = systemTime(CLOCK_MONOTONIC) / 1000;

  // Check if this is a reconnection (gConnectionCount > 1)
  uint32_t connCount;
  {
    std::lock_guard<std::mutex> lock(gConnectionMutex);
    connCount = gConnectionCount;
  }

  ALOGI("  Encoder Loop Started (Connection #%u)", connCount);
  if (gRuntimeConfig.frameLogging) {
    ALOGI("  Frame logging: ENABLED");
  }

  // Send fast-start packet on reconnection (connCount > 1)
  if (connCount > 1) {
    if (!sendFastStartPacket(vsockFd)) {
      ALOGW("Fast-start packet failed, will wait for natural IDR");
    }
  }

  while (!gStopRequested) {
    // Check if encoder is paused (should not happen in this loop, but
    // defensive)
    if (isEncoderPaused()) {
      ALOGW("Encoder paused during loop, exiting");
      return UNKNOWN_ERROR;
    }

    // CSD Heartbeat (every 2 seconds during streaming)
    int64_t nowUs = systemTime(CLOCK_MONOTONIC) / 1000;
    int64_t timeSinceLastHeartbeatUs = nowUs - gLastCsdHeartbeatTimeUs;

    if (timeSinceLastHeartbeatUs >= kCsdHeartbeatIntervalSec * 1000000LL) {
      sp<ABuffer> csdCopy;
      {
        std::lock_guard<std::mutex> lock(gCacheMutex);
        if (gCachedCSDAnnexB && gCachedCSDAnnexB->size() > 0) {
          csdCopy = gCachedCSDAnnexB;
        }
      }

      if (csdCopy) {
        ALOGI("CSD Heartbeat #%u (Interval: %.1fs)", csdHeartbeatCount + 1,
              timeSinceLastHeartbeatUs / 1e6f);

        if (!sendFramedAu(vsockFd, csdCopy->data(), (uint32_t)csdCopy->size(),
                          nowUs, kFrameFlagConfig)) {
          ALOGE("CSD heartbeat send failed (vsock disconnect detected)");
          pauseEncoder();
          return UNKNOWN_ERROR;
        }

        csdSentCount++;
        csdHeartbeatCount++;
        gLastCsdHeartbeatTimeUs = nowUs;
      }
    }

    // Dequeue frame from encoder
    size_t bufIndex, offset, size;
    int64_t ptsUs;
    uint32_t flags;

    err = codec->dequeueOutputBuffer(&bufIndex, &offset, &size, &ptsUs, &flags,
                                     250000);

    switch (err) {
      case NO_ERROR:
        timeoutCount = 0;
        break;

      case -EAGAIN:  // No frame ready yet
        ++timeoutCount;
        if (timeoutCount % 4 == 0) {
          updateDisplayProjection(virtualDpy, displayState);
        }
        if (timeoutCount % 40 == 0 && verbose) {
          ALOGI("Waiting for frames (%u timeouts, %.1fs)", timeoutCount,
                (timeoutCount * 250000LL) / 1e6f);
        }
        usleep(5000);
        continue;

      case android::INFO_FORMAT_CHANGED: {
        // INFO_FORMAT_CHANGED fires ONCE at encoder startup
        // Cache CSD (SPS + PPS) in global variable
        ALOGI("  INFO_FORMAT_CHANGED (First-time CSD caching)");

        sp<AMessage> newFormat;
        codec->getOutputFormat(&newFormat);
        if (verbose) {
          ALOGI("  Format: %s", newFormat->debugString().c_str());
        }

        sp<ABuffer> csd0, csd1;
        bool has0 = newFormat->findBuffer("csd-0", &csd0);
        bool has1 = newFormat->findBuffer("csd-1", &csd1);

        if (!has0) {
          ALOGW("  No csd-0 found (will wait for CODECCONFIG buffer)");
          continue;
        }

        std::vector<uint8_t> annexb;

        // Convert csd-0 (SPS) to Annex-B if needed
        if (hasAnnexBStartCode(csd0->data(), csd0->size())) {
          annexb.insert(annexb.end(), csd0->data(),
                        csd0->data() + csd0->size());
        } else {
          auto conv = avccToAnnexB(csd0->data(), csd0->size());
          annexb.insert(annexb.end(), conv.begin(), conv.end());
        }

        // Convert csd-1 (PPS) to Annex-B if needed
        if (has1) {
          if (hasAnnexBStartCode(csd1->data(), csd1->size())) {
            annexb.insert(annexb.end(), csd1->data(),
                          csd1->data() + csd1->size());
          } else {
            auto conv = avccToAnnexB(csd1->data(), csd1->size());
            annexb.insert(annexb.end(), conv.begin(), conv.end());
          }
        }

        if (!annexb.empty()) {
          sp<ABuffer> newCsd = new ABuffer(annexb.size());
          memcpy(newCsd->data(), annexb.data(), annexb.size());
          newCsd->setRange(0, annexb.size());

          {
            std::lock_guard<std::mutex> lock(gCacheMutex);
            gCachedCSDAnnexB = newCsd;
          }

          ALOGI("  CSD cached: %zu bytes, %d NAL units", annexb.size(),
                countNalUnits(newCsd->data(), newCsd->size()));
        }
        continue;
      }

      case android::INFO_OUTPUT_BUFFERS_CHANGED:
        ALOGI("Output buffers changed (refreshing)");
        err = codec->getOutputBuffers(&buffers);
        if (err != android::OK) {
          ALOGE("Failed to refresh buffers: %d", err);
          pauseEncoder();
          return err;
        }
        continue;

      case INVALID_OPERATION:
        ALOGE("dequeueOutputBuffer: INVALID_OPERATION");
        pauseEncoder();
        return err;

      default:
        ALOGE("dequeueOutputBuffer: unexpected error %d", err);
        pauseEncoder();
        return err;
    }

    // Refresh buffer list if empty
    if (buffers.isEmpty()) {
      ALOGW("Buffers empty (refreshing)");
      err = codec->getOutputBuffers(&buffers);
      if (err != android::OK || buffers.isEmpty()) {
        ALOGE("Still no buffers (skipping frame)");
        codec->releaseOutputBuffer(bufIndex);
        continue;
      }
    }

    sp<MediaCodecBuffer> buffer = buffers.itemAt(bufIndex);
    const uint8_t* data = buffer->data() + offset;

    // Handle CODECCONFIG buffer (fallback CSD delivery)
    if (flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) {
      ALOGI("CODECCONFIG buffer (fallback CSD delivery)");

      std::vector<uint8_t> annexb;
      if (hasAnnexBStartCode(data, size)) {
        annexb.assign(data, data + size);
      } else {
        annexb = avccToAnnexB(data, size);
      }

      if (!annexb.empty()) {
        sp<ABuffer> newCsd = new ABuffer(annexb.size());
        memcpy(newCsd->data(), annexb.data(), annexb.size());
        newCsd->setRange(0, annexb.size());

        {
          std::lock_guard<std::mutex> lock(gCacheMutex);
          gCachedCSDAnnexB = newCsd;
        }

        ALOGI("CSD cached from CODECCONFIG (%zu bytes)", annexb.size());
      }

      codec->releaseOutputBuffer(bufIndex);
      continue;
    }

    // Skip empty buffers
    if (size == 0) {
      codec->releaseOutputBuffer(bufIndex);
      continue;
    }

    // Handle video frame (IDR or P-frame)
    bool isKeyframe = (flags & MediaCodec::BUFFER_FLAG_SYNCFRAME) != 0;
    uint32_t frameFlags = 0;

    if (isKeyframe) {
      frameFlags |= kFrameFlagKeyframe;
      idrCount++;

      // Prepend CSD before every IDR
      sp<ABuffer> csdCopy;
      {
        std::lock_guard<std::mutex> lock(gCacheMutex);
        csdCopy = gCachedCSDAnnexB;
      }

      if (csdCopy && csdCopy->size() > 0) {
        if (gRuntimeConfig.frameLogging) {
          ALOGI("[%u] CONFIG (pre-IDR) %zu bytes pts=%" PRId64, frameCount,
                csdCopy->size(), ptsUs);
        }

        if (!sendFramedAu(vsockFd, csdCopy->data(), (uint32_t)csdCopy->size(),
                          ptsUs, kFrameFlagConfig)) {
          ALOGE("Failed to send CSD before IDR (vsock disconnect detected)");
          codec->releaseOutputBuffer(bufIndex);
          pauseEncoder();
          return UNKNOWN_ERROR;
        }
        csdSentCount++;
      }
    } else {
      pframeCount++;
    }

    // Convert to Annex-B if needed
    const uint8_t* payloadToSend = data;
    uint32_t payloadSize = size;
    std::vector<uint8_t> converted;

    if (!hasAnnexBStartCode(data, size)) {
      converted = avccToAnnexB(data, size);
      if (converted.empty()) {
        ALOGE("AVCC to Annex-B conversion failed for frame #%u", frameCount);
        codec->releaseOutputBuffer(bufIndex);
        continue;
      }
      payloadToSend = converted.data();
      payloadSize = converted.size();
    }

    if (gRuntimeConfig.frameLogging) {
      const char* nalType = getNalTypeName(payloadToSend, payloadSize);
      ALOGI("[%u] %s %u bytes pts=%" PRId64 " NAL=%s", frameCount,
            isKeyframe ? "IDR" : "P", payloadSize, ptsUs, nalType);
    }

    // Send frame over vsock
    if (!sendFramedAu(vsockFd, payloadToSend, payloadSize, ptsUs, frameFlags)) {
      ALOGE("Failed to send frame #%u (vsock disconnect detected)", frameCount);
      codec->releaseOutputBuffer(bufIndex);
      pauseEncoder();
      return UNKNOWN_ERROR;
    }

    // Cache IDR for fast-start recovery
    if (isKeyframe) {
      cacheLatestIDR(payloadToSend, payloadSize, ptsUs);
    }

    frameCount++;
    codec->releaseOutputBuffer(bufIndex);

    // Periodic stats (every 60 frames ~= 1 second at 60fps)
    if (verbose && frameCount % 60 == 0) {
      int64_t nowNs = systemTime(CLOCK_MONOTONIC);
      float fps = frameCount / ((nowNs - startTimeNs) / 1e9f);
      float idrRatio = (float)idrCount / frameCount * 100.0f;
      float avgGop = (idrCount > 0) ? (frameCount / (float)idrCount) : 0.0f;

      ALOGI(
          "Stats: frames=%u IDR=%u (%.1f%% avgGOP=%.1f) P=%u FPS=%.1f CSD=%u "
          "(hb=%u)",
          frameCount, idrCount, idrRatio, avgGop, pframeCount, fps,
          csdSentCount, csdHeartbeatCount);
    }

    // Update projection on keyframe
    if (isKeyframe) {
      updateDisplayProjection(virtualDpy, displayState);
    }

    if (flags & MediaCodec::BUFFER_FLAG_EOS) {
      ALOGI("EOS received from encoder");
      break;
    }
  }

  ALOGI("  Encoder Loop Finished (Connection #%u)", connCount);
  ALOGI("  Total frames:    %u", frameCount);
  ALOGI("  IDR frames:      %u (%.1f%%)", idrCount,
        (float)idrCount / frameCount * 100.0f);
  ALOGI("  P-frames:        %u (%.1f%%)", pframeCount,
        (float)pframeCount / frameCount * 100.0f);
  ALOGI("  CSD sent:        %u times", csdSentCount);
  ALOGI("  CSD heartbeats:  %u", csdHeartbeatCount);
  ALOGI("  Duration:        %.1f seconds",
        (systemTime(CLOCK_MONOTONIC) - startTimeNs) / 1e9f);

  pauseEncoder();
  return gStopRequested ? android::OK : UNKNOWN_ERROR;
}

/**
 * recordScreen - Main entry point: setup encoder and vsock reconnect loop
 *
 * Creates encoder and virtual display once, then enters broker-controlled
 * reconnect loop:
 *   1. Wait for broker to connect (broker connects when AGL is ready)
 *   2. Resume encoder and start streaming
 *   3. On disconnect, pause encoder and wait for next connection
 *   4. On reconnect, send cached CSD+IDR for fast-start
 *
 * Arguments: None
 * Return: NO_ERROR on clean shutdown, error code on failure
 */
static status_t recordScreen() {
  status_t err = configureSignals();
  if (err != NO_ERROR) return err;

  ProcessState::self()->startThreadPool();

  // Get physical display
  PhysicalDisplayId displayId;
  err = getPhysicalDisplayId(displayId);
  if (err != NO_ERROR) return err;

  sp<IBinder> displayToken =
      SurfaceComposerClient::getPhysicalDisplayToken(displayId);
  if (!displayToken) {
    ALOGE("ERROR: failed to get display token\n");
    return UNKNOWN_ERROR;
  }

  ui::DisplayState displayState;
  err = SurfaceComposerClient::getDisplayState(displayToken, &displayState);
  if (err != NO_ERROR) {
    ALOGE("ERROR: unable to get display state\n");
    return err;
  }

  if (displayState.layerStack == ui::INVALID_LAYER_STACK) {
    ALOGE("ERROR: INVALID_LAYER_STACK\n");
    return INVALID_OPERATION;
  }

  ui::DisplayMode displayMode;
  err = SurfaceComposerClient::getActiveDisplayMode(displayToken, &displayMode);
  float displayFps = (err == NO_ERROR) ? displayMode.refreshRate : 60.0f;

  // Create encoder once (lives across reconnects)
  sp<MediaCodec> codec;
  sp<IGraphicBufferProducer> bufferProducer;
  err = prepareEncoder(displayFps, &codec, &bufferProducer);
  if (err != NO_ERROR) return err;

  // Create virtual display once (mirror continues even when encoder paused)
  sp<IBinder> dpy;
  sp<SurfaceControl> mirrorRoot;
  err = prepareVirtualDisplay(displayState, bufferProducer, &dpy, &mirrorRoot);
  if (err != NO_ERROR) {
    codec->stop();
    codec->release();
    return err;
  }

  usleep(1000000);  // 1s for SurfaceFlinger to initialize mirror
  updateDisplayProjection(dpy, displayState);

  // Broker-controlled reconnect loop
  // AAOS listens on gVsockConfig.port, broker initiates connection when AGL is
  // ready
  //
  // Architecture:
  //   1. AGL connects to broker (CID=2, PORT=5000)
  //   2. Broker detects AGL connection
  //   3. Broker initiates connection to AAOS (CID=3, PORT=22345)
  //   4. AAOS accepts connection and starts streaming
  //   5. On AGL disconnect, broker closes AAOS connection
  //   6. AAOS pauses encoder and waits for next broker connection

  // Create listening socket once
  int listenFd = VsockUtils::createVsockListener(gVsockConfig.port);
  if (listenFd < 0) {
    ALOGE("Failed to create vsock listener on port %u: %s", gVsockConfig.port,
          strerror(errno));
    SurfaceComposerClient::destroyDisplay(dpy);
    codec->stop();
    codec->release();
    return UNKNOWN_ERROR;
  }
  ALOGI("Listening for broker connections on vsock port %u", gVsockConfig.port);

  while (!gStopRequested) {
    ALOGI("Waiting for broker connection (AGL ready signal)...");

    int vsockFd = VsockUtils::acceptConnection(listenFd);
    if (vsockFd < 0) {
      if (errno == EINTR) continue;  // Interrupted by signal, retry
      ALOGE("vsock accept failed (%s), retrying in 2s", strerror(errno));
      sleep(2);
      continue;
    }
    ALOGI("Broker connected (fd=%d) - AGL is ready", vsockFd);

    // Reset heartbeat timer on connection
    gLastCsdHeartbeatTimeUs = systemTime(CLOCK_MONOTONIC) / 1000;

    // Run encoder (will pause on disconnect)
    err = runEncoder(codec, vsockFd, gRuntimeConfig.verbose, dpy, displayState);

    close(vsockFd);
    ALOGI("Broker disconnected (fd closed)");

    if (gStopRequested || err == android::OK) break;

    ALOGI("Encoder paused, waiting for next broker connection...");
  }

  // Cleanup
  close(listenFd);
  ALOGI("Closed listening socket");
  SurfaceComposerClient::destroyDisplay(dpy);
  codec->stop();
  codec->release();

  return gStopRequested ? android::OK : err;
}

/**
 * parseWidthHeight - Parse resolution string (e.g., "854x480")
 *
 * Arguments:
 *   @param s Input string
 *   @param w Output: Width
 *   @param h Output: Height
 * Return:
 *   true on success, false on parse error
 */
static bool parseWidthHeight(const char* s, uint32_t* w, uint32_t* h) {
  char* end;
  long lw = strtol(s, &end, 10);
  if (end == s || *end != 'x' || !*(end + 1)) return false;
  long lh = strtol(end + 1, &end, 10);
  if (*end) return false;
  *w = (uint32_t)lw;
  *h = (uint32_t)lh;
  return true;
}

/**
 * parseValueWithUnit - Parse bitrate with optional 'M' suffix
 *
 * Arguments:
 *   @param str Input string (e.g., "1.5M" or "1500000")
 *   @param pValue Output: Parsed value
 * Return:
 *   NO_ERROR on success, UNKNOWN_ERROR on parse error
 */
static status_t parseValueWithUnit(const char* str, uint32_t* pValue) {
  char* endptr;
  long value = strtol(str, &endptr, 10);
  if (!*endptr) {
    *pValue = (uint32_t)value;
    return NO_ERROR;
  }
  if (toupper(*endptr) == 'M' && !*(endptr + 1)) {
    *pValue = (uint32_t)(value * 1000000);
    return NO_ERROR;
  }
  ALOGE("Unrecognized value: %s\n", str);
  return UNKNOWN_ERROR;
}

/**
 * usage - Print command-line usage information
 *
 * Arguments: None
 * Return: None
 */
static void usage() {
  fprintf(stderr,
          "screenmirror - Broker-controlled H.264 vsock streaming\n"
          "\n"
          "Defaults: %ux%u @ 60fps, %.2fMbps, GOP=2s\n"
          "\n"
          "Options:\n"
          "  --size WxH         Video size (default: %ux%u)\n"
          "  --bit-rate RATE    Bitrate ('1.5M' or '1500000')\n"
          "  --vsock PORT       Listen port (default: %u)\n"
          "  --frame-log        Enable per-frame logging\n"
          "  --verbose          Enable verbose logging\n"
          "  --help             This message\n"
          "\n",
          gEncoderConfig.width, gEncoderConfig.height,
          gEncoderConfig.bitrate / 1000000.0, gEncoderConfig.width,
          gEncoderConfig.height, gVsockConfig.port);
}

/**
 * main - Entry point: parse arguments and start screen recording
 *
 * Arguments:
 *   @param argc Argument count
 *   @param argv Argument vector
 * Return:
 *   0 on success, non-zero on failure
 */
int main(int argc, char** argv) {
  static const struct option opts[] = {
      {"help", no_argument, NULL, 'h'},
      {"verbose", no_argument, NULL, 'v'},
      {"frame-log", no_argument, NULL, 'f'},
      {"size", required_argument, NULL, 's'},
      {"bit-rate", required_argument, NULL, 'b'},
      {"vsock", required_argument, NULL, 'V'},
      {"codec-name", required_argument, NULL, 'N'},
      {"persistent-surface", no_argument, NULL, 'p'},
      {"display-id", required_argument, NULL, 'd'},
      {NULL, 0, NULL, 0}};

  while (true) {
    int idx = 0, c = getopt_long(argc, argv, "", opts, &idx);
    if (c == -1) break;
    switch (c) {
      case 'h':
        usage();
        return 0;
      case 'v':
        gRuntimeConfig.verbose = true;
        break;
      case 'f':
        gRuntimeConfig.frameLogging = true;
        gRuntimeConfig.verbose = true;
        break;
      case 's':
        if (!parseWidthHeight(optarg, &gEncoderConfig.width,
                              &gEncoderConfig.height)) {
          ALOGE("Invalid size '%s'\n", optarg);
          return 2;
        }
        if (!gEncoderConfig.width || !gEncoderConfig.height) {
          ALOGE("Size cannot be zero\n");
          return 2;
        }
        gEncoderConfig.sizeSpecified = true;
        break;
      case 'b':
        if (parseValueWithUnit(optarg, &gEncoderConfig.bitrate) != NO_ERROR)
          return 2;
        if (gEncoderConfig.bitrate < kMinBitRate ||
            gEncoderConfig.bitrate > kMaxBitRate) {
          ALOGE("Bitrate %u out of range [%u,%u]\n", gEncoderConfig.bitrate,
                kMinBitRate, kMaxBitRate);
          return 2;
        }
        break;
      case 'V':
        gVsockConfig.enabled = true;
        gVsockConfig.port = (uint32_t)strtol(optarg, NULL, 10);
        if (!gVsockConfig.port) {
          ALOGE("Invalid port\n");
          return 2;
        }
        break;
      case 'N':
        gRuntimeConfig.codecName = optarg;
        break;
      case 'p':
        gRuntimeConfig.persistentSurface = true;
        break;
      case 'd':
        if (auto id =
                android::DisplayId::fromValue<PhysicalDisplayId>(atoll(optarg));
            id && SurfaceComposerClient::getPhysicalDisplayToken(*id)) {
          gPhysicalDisplayId = *id;
          break;
        }
        ALOGE("Invalid display ID\n");
        return 2;
      default:
        return 2;
    }
  }

  if (!gVsockConfig.enabled) {
    ALOGE("Must specify --vsock\n");
    return 2;
  }

  status_t err = recordScreen();
  ALOGD(err == NO_ERROR ? "success" : "failed");
  return (int)err;
}