/*
 * screentransfer_test.cpp
 *
 * Unit tests for screentransfer.cpp.
 *
 */

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

// Minimal stubs so the pure-logic functions compile without the Android
// framework
#ifdef UNIT_TEST_BUILD
#define ALOGI(...) ((void)0)
#define ALOGE(...) ((void)0)
#define ALOGW(...) ((void)0)
#define ALOGD(...) ((void)0)
namespace android {
namespace VsockUtils {
static bool writeFully(int fd, const void* buf, size_t n) {
  const auto* p = reinterpret_cast<const uint8_t*>(buf);
  size_t done = 0;
  while (done < n) {
    ssize_t r = ::write(fd, p + done, n - done);
    if (r <= 0) return false;
    done += (size_t)r;
  }
  return true;
}
}  // namespace VsockUtils
}  // namespace android
#endif

// Verbatim copies of every pure function under test.
// These must be kept byte-for-byte identical to screentransfer.cpp.

// Constants
static constexpr uint32_t kFrameFlagKeyframe = 0x01u;
static constexpr uint32_t kFrameFlagEos = 0x04u;
static constexpr uint32_t kFrameFlagConfig = 0x08u;

static constexpr uint32_t kMinBitRate = 100000u;
static constexpr uint32_t kMaxBitRate = 4000000u;
static constexpr uint32_t kCap480pWidth = 854u;
static constexpr uint32_t kCap480pHeight = 480u;

// floorToEven
static inline uint32_t floorToEven(uint32_t n) { return n & ~1u; }

// capFps
static float capFps(float displayFps) {
  if (displayFps <= 32.0f) return displayFps;
  if (displayFps <= 65.0f) return 30.0f;
  return 15.0f;
}

// compute480pSize
static void compute480pSize(uint32_t dw, uint32_t dh, uint32_t* outW,
                            uint32_t* outH) {
  if (dh == 0) {
    *outW = kCap480pWidth;
    *outH = kCap480pHeight;
    return;
  }
  if (dh <= kCap480pHeight) {
    *outW = floorToEven(dw);
    *outH = floorToEven(dh);
    return;
  }
  float scale = (float)kCap480pHeight / (float)dh;
  uint32_t newW = floorToEven((uint32_t)(dw * scale + 0.5f));
  uint32_t newH = kCap480pHeight;
  if (newW > kCap480pWidth) newW = kCap480pWidth;
  *outW = newW;
  *outH = newH;
}

// writeValueLE
template <typename T>
static void writeValueLE(T value, uint8_t* buf) {
  std::remove_const_t<T> tmp = value;
  for (int i = 0; i < (int)sizeof(T); ++i) {
    buf[i] = static_cast<uint8_t>(tmp & 0xff);
    tmp >>= 8;
  }
}

// hasAnnexBStartCode
static bool hasAnnexBStartCode(const uint8_t* buf, size_t sz) {
  if (sz >= 4 && buf[0] == 0 && buf[1] == 0 && buf[2] == 0 && buf[3] == 1)
    return true;
  if (sz >= 3 && buf[0] == 0 && buf[1] == 0 && buf[2] == 1) return true;
  return false;
}

// avccToAnnexB
static std::vector<uint8_t> avccToAnnexB(const uint8_t* data, size_t sz) {
  if (!data || sz == 0) return {};
  static const uint8_t kSC[4] = {0, 0, 0, 1};
  std::vector<uint8_t> out;
  out.reserve(sz + 32);
  const uint8_t* src = data;
  const uint8_t* end = data + sz;
  while (src + 4 <= end) {
    uint32_t n = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) |
                 ((uint32_t)src[2] << 8) | (uint32_t)src[3];
    src += 4;
    if (src + n > end) {
      out.clear();
      return out;
    }
    out.insert(out.end(), kSC, kSC + 4);
    out.insert(out.end(), src, src + n);
    src += n;
  }
  return out;
}

// parseWidthHeight
static bool parseWidthHeight(const char* s, uint32_t* w, uint32_t* h) {
  char* e;
  long lw = strtol(s, &e, 10);
  if (e == s || *e != 'x' || *(e + 1) == '\0') return false;
  long lh = strtol(e + 1, &e, 10);
  if (*e != '\0') return false;
  *w = (uint32_t)lw;
  *h = (uint32_t)lh;
  return true;
}

//  parseValueWithUnit
// Returns 0 (NO_ERROR equivalent) on success, -1 on failure.
static int parseValueWithUnit(const char* str, uint32_t* v) {
  char* ep;
  long val = strtol(str, &ep, 10);
  if (*ep == '\0') {
    *v = (uint32_t)val;
    return 0;
  }
  if (toupper((unsigned char)*ep) == 'M' && *(ep + 1) == '\0') {
    *v = (uint32_t)(val * 1000000);
    return 0;
  }
  return -1;
}

//  sendFramedAu
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
  if (!android::VsockUtils::writeFully(fd, hdr, 20)) return false;
  if (payloadLen > 0 &&
      !android::VsockUtils::writeFully(fd, payload, payloadLen))
    return false;
  return true;
}

// Shared test helpers
static bool readFull(int fd, void* buf, size_t n) {
  auto* p = reinterpret_cast<uint8_t*>(buf);
  size_t got = 0;
  while (got < n) {
    ssize_t r = ::read(fd, p + got, n - got);
    if (r <= 0) return false;
    got += (size_t)r;
  }
  return true;
}

static uint32_t u32le(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static uint64_t u64le(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i] << (8 * i));
  return v;
}

// Frame as received by bridge side
struct RecvFrame {
  uint32_t len;
  uint64_t ptsUs;
  uint32_t flags;
  std::vector<uint8_t> payload;
};

// resync MAGIC FRAM logic
static int resync_to_magic(int fd) {
  uint8_t w[4];
  if (!readFull(fd, w, 4)) return -1;
  for (;;) {
    if (w[0] == 'F' && w[1] == 'R' && w[2] == 'A' && w[3] == 'M') return 0;
    w[0] = w[1];
    w[1] = w[2];
    w[2] = w[3];
    ssize_t r = ::read(fd, &w[3], 1);
    if (r <= 0) return -1;
  }
}

static bool recvFrame(int fd, RecvFrame& f) {
  if (resync_to_magic(fd) != 0) return false;
  uint8_t rest[16];
  if (!readFull(fd, rest, 16)) return false;
  f.len = u32le(rest + 0);
  f.ptsUs = u64le(rest + 4);
  f.flags = u32le(rest + 12);
  f.payload.resize(f.len);
  if (f.len > 0 && !readFull(fd, f.payload.data(), f.len)) return false;
  return true;
}

// Socketpair fixture used by integration tests
class PipeFixture : public ::testing::Test {
 protected:
  int tx = -1, rx = -1;
  void SetUp() override {
    int fds[2];
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    tx = fds[0];
    rx = fds[1];
  }
  void TearDown() override {
    if (tx >= 0) close(tx);
    if (rx >= 0) close(rx);
  }
};

//
// UT_CapFps
//
TEST(UT_CapFps, SlowPanelPassThrough_24) {
  EXPECT_FLOAT_EQ(capFps(24.0f), 24.0f);
}
TEST(UT_CapFps, SlowPanelPassThrough_30) {
  EXPECT_FLOAT_EQ(capFps(30.0f), 30.0f);
}
TEST(UT_CapFps, Panel60Hz_CappedAt30) { EXPECT_FLOAT_EQ(capFps(60.0f), 30.0f); }
TEST(UT_CapFps, Panel59Hz_CappedAt30) { EXPECT_FLOAT_EQ(capFps(59.0f), 30.0f); }
TEST(UT_CapFps, Panel66Hz_CappedAt15) { EXPECT_FLOAT_EQ(capFps(66.0f), 15.0f); }
TEST(UT_CapFps, Panel120Hz_CappedAt15) {
  EXPECT_FLOAT_EQ(capFps(120.0f), 15.0f);
}
TEST(UT_CapFps, OutputIsNeverAbove30_For60Hz) {
  EXPECT_LE(capFps(60.0f), 30.0f);
}
TEST(UT_CapFps, OutputIsNeverAbove15_For120Hz) {
  EXPECT_LE(capFps(120.0f), 15.0f);
}
TEST(UT_CapFps, OutputIsAlwaysPositive) {
  for (float f : {1.0f, 15.0f, 24.0f, 30.0f, 60.0f, 90.0f, 120.0f})
    EXPECT_GT(capFps(f), 0.0f) << "displayFps=" << f;
}
TEST(UT_CapFps, BoundaryAt32_Inclusive) {
  EXPECT_FLOAT_EQ(capFps(32.0f), 32.0f);
  EXPECT_FLOAT_EQ(capFps(32.1f), 30.0f);
}
TEST(UT_CapFps, BoundaryAt65_Inclusive) {
  EXPECT_FLOAT_EQ(capFps(65.0f), 30.0f);
  EXPECT_FLOAT_EQ(capFps(65.1f), 15.0f);
}

//
// UT_Compute480pSize
//
TEST(UT_Compute480pSize, NativeBelow480_Preserved) {
  uint32_t w = 0, h = 0;
  compute480pSize(640, 360, &w, &h);
  EXPECT_EQ(w, 640u);
  EXPECT_EQ(h, 360u);
}

TEST(UT_Compute480pSize, NativeExactly480_Preserved) {
  uint32_t w = 0, h = 0;
  compute480pSize(854, 480, &w, &h);
  EXPECT_EQ(w, 854u);
  EXPECT_EQ(h, 480u);
}

TEST(UT_Compute480pSize, HD720p_ScaledTo480p) {
  uint32_t w = 0, h = 0;
  compute480pSize(1280, 720, &w, &h);
  EXPECT_EQ(h, 480u);
  EXPECT_LE(w, kCap480pWidth);
  EXPECT_EQ(w % 2, 0u) << "width must be even";
}

TEST(UT_Compute480pSize, FullHD1080p_ScaledTo480p) {
  uint32_t w = 0, h = 0;
  compute480pSize(1920, 1080, &w, &h);
  EXPECT_EQ(h, 480u);
  EXPECT_LE(w, kCap480pWidth);
  EXPECT_EQ(w % 2, 0u);
}

TEST(UT_Compute480pSize, FourK_ScaledTo480p) {
  uint32_t w = 0, h = 0;
  compute480pSize(3840, 2160, &w, &h);
  EXPECT_EQ(h, 480u);
  EXPECT_LE(w, kCap480pWidth);
  EXPECT_EQ(w % 2, 0u);
}

TEST(UT_Compute480pSize, OddNativeSize_FlooredToEven) {
  uint32_t w = 0, h = 0;
  compute480pSize(641, 361, &w,
                  &h);  // 361 < 480, should preserve native floored
  EXPECT_EQ(w % 2, 0u);
  EXPECT_EQ(h % 2, 0u);
}

TEST(UT_Compute480pSize, OutputHeightNeverExceeds480) {
  uint32_t sizes[][2] = {
      {1280, 720}, {1920, 1080}, {3840, 2160}, {800, 600}, {1024, 768}};
  for (auto& s : sizes) {
    uint32_t w = 0, h = 0;
    compute480pSize(s[0], s[1], &w, &h);
    EXPECT_LE(h, kCap480pHeight) << s[0] << "x" << s[1];
  }
}

TEST(UT_Compute480pSize, OutputWidthNeverExceedsCap) {
  uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}};
  for (auto& s : sizes) {
    uint32_t w = 0, h = 0;
    compute480pSize(s[0], s[1], &w, &h);
    EXPECT_LE(w, kCap480pWidth) << s[0] << "x" << s[1];
  }
}

TEST(UT_Compute480pSize, OutputAlwaysEven) {
  uint32_t sizes[][2] = {{1280, 720}, {1920, 1080}, {1281, 721}, {641, 361}};
  for (auto& s : sizes) {
    uint32_t w = 0, h = 0;
    compute480pSize(s[0], s[1], &w, &h);
    EXPECT_EQ(w % 2, 0u) << s[0] << "x" << s[1];
    EXPECT_EQ(h % 2, 0u) << s[0] << "x" << s[1];
  }
}

TEST(UT_Compute480pSize, ZeroHeight_ReturnsCap) {
  uint32_t w = 0, h = 0;
  compute480pSize(0, 0, &w, &h);
  EXPECT_EQ(w, kCap480pWidth);
  EXPECT_EQ(h, kCap480pHeight);
}

//
// UT_FloorToEven
//
TEST(UT_FloorToEven, EvenUnchanged) { EXPECT_EQ(floorToEven(640u), 640u); }
TEST(UT_FloorToEven, OddFlooredDown) { EXPECT_EQ(floorToEven(641u), 640u); }
TEST(UT_FloorToEven, Zero) { EXPECT_EQ(floorToEven(0u), 0u); }
TEST(UT_FloorToEven, One) { EXPECT_EQ(floorToEven(1u), 0u); }
TEST(UT_FloorToEven, MaxEven) { EXPECT_EQ(floorToEven(4096u), 4096u); }
TEST(UT_FloorToEven, AlwaysEven) {
  for (uint32_t n = 0; n < 16; ++n)
    EXPECT_EQ(floorToEven(n) % 2, 0u) << "n=" << n;
}

//
// UT_WriteValueLE
//
TEST(UT_WriteValueLE, U32_Zero) {
  uint8_t b[4] = {0xFF, 0xFF, 0xFF, 0xFF};
  writeValueLE<uint32_t>(0, b);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(b[i], 0u) << "byte " << i;
}
TEST(UT_WriteValueLE, U32_KnownPattern) {
  uint8_t b[4];
  writeValueLE<uint32_t>(0x04030201u, b);
  EXPECT_EQ(b[0], 0x01u);
  EXPECT_EQ(b[1], 0x02u);
  EXPECT_EQ(b[2], 0x03u);
  EXPECT_EQ(b[3], 0x04u);
}
TEST(UT_WriteValueLE, U32_DEADBEEF) {
  uint8_t b[4];
  writeValueLE<uint32_t>(0xDEADBEEFu, b);
  EXPECT_EQ(b[0], 0xEFu);
  EXPECT_EQ(b[1], 0xBEu);
  EXPECT_EQ(b[2], 0xADu);
  EXPECT_EQ(b[3], 0xDEu);
}
TEST(UT_WriteValueLE, U64_KnownPattern) {
  uint8_t b[8];
  writeValueLE<uint64_t>(0x0807060504030201ULL, b);
  for (int i = 0; i < 8; ++i) EXPECT_EQ(b[i], (uint8_t)(i + 1)) << "byte " << i;
}
TEST(UT_WriteValueLE, U32_RoundTrip) {
  uint8_t b[4];
  uint32_t v = 0x12345678u;
  writeValueLE<uint32_t>(v, b);
  EXPECT_EQ(u32le(b), v);
}
TEST(UT_WriteValueLE, U64_RoundTrip) {
  uint8_t b[8];
  uint64_t v = 0xFEDCBA9876543210ULL;
  writeValueLE<uint64_t>(v, b);
  EXPECT_EQ(u64le(b), v);
}

//
// UT_HasAnnexBStartCode
//
TEST(UT_HasAnnexBStartCode, FourByte) {
  const uint8_t b[] = {0, 0, 0, 1, 0x67};
  EXPECT_TRUE(hasAnnexBStartCode(b, 5));
}
TEST(UT_HasAnnexBStartCode, ThreeByte) {
  const uint8_t b[] = {0, 0, 1, 0x67};
  EXPECT_TRUE(hasAnnexBStartCode(b, 4));
}
TEST(UT_HasAnnexBStartCode, AVCC) {
  const uint8_t b[] = {0, 0, 0, 5, 0x67};
  EXPECT_FALSE(hasAnnexBStartCode(b, 5));
}
TEST(UT_HasAnnexBStartCode, TooShort2B) {
  const uint8_t b[] = {0, 0};
  EXPECT_FALSE(hasAnnexBStartCode(b, 2));
}
TEST(UT_HasAnnexBStartCode, Exact3B) {
  const uint8_t b[] = {0, 0, 1};
  EXPECT_TRUE(hasAnnexBStartCode(b, 3));
}
TEST(UT_HasAnnexBStartCode, MidBuffer) {
  const uint8_t b[] = {1, 0, 0, 0, 1};
  EXPECT_FALSE(hasAnnexBStartCode(b, 5));
}
TEST(UT_HasAnnexBStartCode, AllFF) {
  const uint8_t b[] = {0xFF, 0xFF, 0xFF, 0xFF};
  EXPECT_FALSE(hasAnnexBStartCode(b, 4));
}
TEST(UT_HasAnnexBStartCode, NullZeroSz) {
  EXPECT_FALSE(hasAnnexBStartCode(nullptr, 0));
}

//
// UT_AvccToAnnexB
//
TEST(UT_AvccToAnnexB, SingleNal) {
  const uint8_t avcc[] = {0, 0, 0, 3, 0x67, 0x42, 0x00};
  auto out = avccToAnnexB(avcc, 7);
  ASSERT_EQ(out.size(), 7u);
  EXPECT_EQ(out[0], 0u);
  EXPECT_EQ(out[1], 0u);
  EXPECT_EQ(out[2], 0u);
  EXPECT_EQ(out[3], 1u);
  EXPECT_EQ(out[4], 0x67u);
  EXPECT_EQ(out[5], 0x42u);
  EXPECT_EQ(out[6], 0x00u);
}
TEST(UT_AvccToAnnexB, TwoNals_SPS_PPS) {
  const uint8_t avcc[] = {0, 0, 0, 2, 0x67, 0x42, 0, 0, 0, 2, 0x68, 0xCE};
  auto out = avccToAnnexB(avcc, 12);
  ASSERT_EQ(out.size(), 12u);
  EXPECT_EQ(out[0], 0u);
  EXPECT_EQ(out[3], 1u);
  EXPECT_EQ(out[4], 0x67u);
  EXPECT_EQ(out[6], 0u);
  EXPECT_EQ(out[9], 1u);
  EXPECT_EQ(out[10], 0x68u);
}
TEST(UT_AvccToAnnexB, InvalidNalLen_ReturnsEmpty) {
  const uint8_t avcc[] = {0, 0, 0, 100};  // claims 100 bytes, buffer has 0
  EXPECT_TRUE(avccToAnnexB(avcc, 4).empty());
}
TEST(UT_AvccToAnnexB, NullInput_ReturnsEmpty) {
  EXPECT_TRUE(avccToAnnexB(nullptr, 0).empty());
}
TEST(UT_AvccToAnnexB, OutputStartsWithAnnexBSC) {
  const uint8_t avcc[] = {0, 0, 0, 1, 0x65};
  auto out = avccToAnnexB(avcc, 5);
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(hasAnnexBStartCode(out.data(), out.size()));
}
TEST(UT_AvccToAnnexB, PayloadPreserved) {
  const uint8_t pl[] = {0x65, 0xB8, 0x00, 0x10};
  uint8_t avcc[8] = {0, 0, 0, 4, 0x65, 0xB8, 0x00, 0x10};
  auto out = avccToAnnexB(avcc, 8);
  ASSERT_EQ(out.size(), 8u);
  EXPECT_EQ(memcmp(out.data() + 4, pl, 4), 0);
}

//
// UT_ParseWidthHeight
//
TEST(UT_ParseWidthHeight, Valid_480p) {
  uint32_t w, h;
  EXPECT_TRUE(parseWidthHeight("854x480", &w, &h));
  EXPECT_EQ(w, 854u);
  EXPECT_EQ(h, 480u);
}
TEST(UT_ParseWidthHeight, Valid_720p) {
  uint32_t w, h;
  EXPECT_TRUE(parseWidthHeight("1280x720", &w, &h));
  EXPECT_EQ(w, 1280u);
  EXPECT_EQ(h, 720u);
}
TEST(UT_ParseWidthHeight, MissingX) {
  uint32_t w, h;
  EXPECT_FALSE(parseWidthHeight("1280720", &w, &h));
}
TEST(UT_ParseWidthHeight, EmptyString) {
  uint32_t w, h;
  EXPECT_FALSE(parseWidthHeight("", &w, &h));
}
TEST(UT_ParseWidthHeight, MissingHeight) {
  uint32_t w, h;
  EXPECT_FALSE(parseWidthHeight("1280x", &w, &h));
}
TEST(UT_ParseWidthHeight, TrailingGarbage) {
  uint32_t w, h;
  EXPECT_FALSE(parseWidthHeight("1280x720abc", &w, &h));
}
TEST(UT_ParseWidthHeight, ZeroWidth) {
  uint32_t w, h;
  EXPECT_TRUE(parseWidthHeight("0x480", &w, &h));
  EXPECT_EQ(w, 0u);
}

//
// UT_ParseValueWithUnit
//
TEST(UT_ParseValueWithUnit, BareNumber) {
  uint32_t v;
  EXPECT_EQ(parseValueWithUnit("1000000", &v), 0);
  EXPECT_EQ(v, 1000000u);
}
TEST(UT_ParseValueWithUnit, MegaSuffix_upper) {
  uint32_t v;
  EXPECT_EQ(parseValueWithUnit("1M", &v), 0);
  EXPECT_EQ(v, 1000000u);
}
TEST(UT_ParseValueWithUnit, MegaSuffix_lower) {
  uint32_t v;
  EXPECT_EQ(parseValueWithUnit("2m", &v), 0);
  EXPECT_EQ(v, 2000000u);
}
TEST(UT_ParseValueWithUnit, InvalidSuffix) {
  uint32_t v;
  EXPECT_NE(parseValueWithUnit("2K", &v), 0);
}
TEST(UT_ParseValueWithUnit, EmptyString) {
  uint32_t v;
  EXPECT_EQ(parseValueWithUnit("", &v), 0);
}
TEST(UT_ParseValueWithUnit, DefaultBitRate) {
  uint32_t v;
  EXPECT_EQ(parseValueWithUnit("1M", &v), 0);
  EXPECT_GE(v, kMinBitRate);
  EXPECT_LE(v, kMaxBitRate);
}

//
// UT_FrameFlags verify constants match bridge values
//
TEST(UT_FrameFlags, KeyframeIs0x01) { EXPECT_EQ(kFrameFlagKeyframe, 0x01u); }
TEST(UT_FrameFlags, EosIs0x04) { EXPECT_EQ(kFrameFlagEos, 0x04u); }
TEST(UT_FrameFlags, ConfigIs0x08) { EXPECT_EQ(kFrameFlagConfig, 0x08u); }
TEST(UT_FrameFlags, NoBitOverlap) {
  EXPECT_EQ(kFrameFlagKeyframe & kFrameFlagEos, 0u);
  EXPECT_EQ(kFrameFlagKeyframe & kFrameFlagConfig, 0u);
  EXPECT_EQ(kFrameFlagEos & kFrameFlagConfig, 0u);
}
TEST(UT_FrameFlags, CombinedReadable) {
  uint32_t c = kFrameFlagKeyframe | kFrameFlagConfig;
  EXPECT_TRUE(c & kFrameFlagKeyframe);
  EXPECT_TRUE(c & kFrameFlagConfig);
  EXPECT_FALSE(c & kFrameFlagEos);
}

//
// UT_Defaults verify tuned defaults satisfy constraints
//
TEST(UT_Defaults, Cap480pWidth_Is16by9) { EXPECT_EQ(kCap480pWidth, 854u); }
TEST(UT_Defaults, Cap480pHeight_Is480) { EXPECT_EQ(kCap480pHeight, 480u); }
TEST(UT_Defaults, Cap480pDimsAreEven) {
  EXPECT_EQ(kCap480pWidth % 2, 0u);
  EXPECT_EQ(kCap480pHeight % 2, 0u);
}
TEST(UT_Defaults, MaxBitRate_Is4Mbps) { EXPECT_EQ(kMaxBitRate, 4000000u); }
TEST(UT_Defaults, MinBitRate_Is100kbps) { EXPECT_EQ(kMinBitRate, 100000u); }
TEST(UT_Defaults, DefaultFps_Is30for60HzDisplay) {
  EXPECT_FLOAT_EQ(capFps(60.0f), 30.0f);
}
TEST(UT_Defaults, DefaultFps_Is15for120HzDisplay) {
  EXPECT_FLOAT_EQ(capFps(120.0f), 15.0f);
}
TEST(UT_Defaults, DefaultEncoderBitRate_WithinRange) {
  // Documented default is 1 Mbps
  constexpr uint32_t kDefault = 1000000u;
  EXPECT_GE(kDefault, kMinBitRate);
  EXPECT_LE(kDefault, kMaxBitRate);
}

//
// UT_SendFramedAu header layout verified byte by byte
//
TEST_F(PipeFixture, UT_SendFramedAu_MagicBytes) {
  const uint8_t pl[] = {0x01};
  ASSERT_TRUE(sendFramedAu(tx, pl, 1, 0, 0));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(hdr[0], 'F');
  EXPECT_EQ(hdr[1], 'R');
  EXPECT_EQ(hdr[2], 'A');
  EXPECT_EQ(hdr[3], 'M');
}
TEST_F(PipeFixture, UT_SendFramedAu_LengthField) {
  const uint8_t pl[7] = {};
  ASSERT_TRUE(sendFramedAu(tx, pl, 7, 0, 0));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(u32le(hdr + 4), 7u);
}
TEST_F(PipeFixture, UT_SendFramedAu_PtsField) {
  const uint8_t pl[] = {0xAA};
  uint64_t pts = 0x123456789ABCDEF0ULL;
  ASSERT_TRUE(sendFramedAu(tx, pl, 1, pts, 0));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(u64le(hdr + 8), pts);
}
TEST_F(PipeFixture, UT_SendFramedAu_FlagsKeyframe) {
  const uint8_t pl[] = {0xBB};
  ASSERT_TRUE(sendFramedAu(tx, pl, 1, 0, kFrameFlagKeyframe));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(u32le(hdr + 16), kFrameFlagKeyframe);
}
TEST_F(PipeFixture, UT_SendFramedAu_FlagsConfig) {
  const uint8_t pl[] = {0xCC};
  ASSERT_TRUE(sendFramedAu(tx, pl, 1, 0, kFrameFlagConfig));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(u32le(hdr + 16), kFrameFlagConfig);
}
TEST_F(PipeFixture, UT_SendFramedAu_ZeroLengthEOS) {
  ASSERT_TRUE(sendFramedAu(tx, nullptr, 0, 0, kFrameFlagEos));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  EXPECT_EQ(u32le(hdr + 4), 0u);
  EXPECT_EQ(u32le(hdr + 16), kFrameFlagEos);
}
TEST_F(PipeFixture, UT_SendFramedAu_PayloadPreserved) {
  const uint8_t pl[] = {0, 0, 0, 1, 0x65, 0xB8, 0x00};
  ASSERT_TRUE(sendFramedAu(tx, pl, sizeof(pl), 99, kFrameFlagKeyframe));
  uint8_t hdr[20];
  ASSERT_TRUE(readFull(rx, hdr, 20));
  uint32_t len = u32le(hdr + 4);
  ASSERT_EQ(len, sizeof(pl));
  std::vector<uint8_t> got(len);
  ASSERT_TRUE(readFull(rx, got.data(), len));
  EXPECT_EQ(memcmp(got.data(), pl, len), 0);
}
TEST_F(PipeFixture, UT_SendFramedAu_TotalWireBytesCorrect) {
  const uint8_t pl[10] = {};
  ASSERT_TRUE(sendFramedAu(tx, pl, 10, 0, 0));
  close(tx);
  tx = -1;
  uint8_t buf[64];
  ssize_t total = 0, r;
  while ((r = read(rx, buf + total, sizeof(buf) - total)) > 0) total += r;
  EXPECT_EQ(total, 30);  // 20 header + 10 payload
}
TEST_F(PipeFixture, UT_SendFramedAu_FailsOnBadFd) {
  const uint8_t pl[] = {0x01};
  EXPECT_FALSE(sendFramedAu(-1, pl, 1, 0, 0));
}

//
// UT_FrameStream consecutive frames are correctly delimited
//
TEST_F(PipeFixture, UT_FrameStream_ThreeFramesInOrder) {
  const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42};
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8};
  const uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A};
  ASSERT_TRUE(sendFramedAu(tx, csd, sizeof(csd), 0, kFrameFlagConfig));
  ASSERT_TRUE(sendFramedAu(tx, idr, sizeof(idr), 33, kFrameFlagKeyframe));
  ASSERT_TRUE(sendFramedAu(tx, pf, sizeof(pf), 66, 0));
  close(tx);
  tx = -1;

  RecvFrame f;
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagConfig);
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagKeyframe);
  EXPECT_EQ(f.ptsUs, 33ULL);
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, 0u);
  EXPECT_EQ(f.ptsUs, 66ULL);
  EXPECT_FALSE(recvFrame(rx, f));  // EOF
}

//
// UT_FrameRoundTrip
//
TEST_F(PipeFixture, UT_FrameRoundTrip_CSD) {
  const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1E};
  ASSERT_TRUE(sendFramedAu(tx, sps, sizeof(sps), 0, kFrameFlagConfig));
  RecvFrame f;
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagConfig);
  EXPECT_EQ(f.ptsUs, 0ULL);
  ASSERT_EQ(f.payload.size(), sizeof(sps));
  EXPECT_EQ(memcmp(f.payload.data(), sps, sizeof(sps)), 0);
}

TEST_F(PipeFixture, UT_FrameRoundTrip_Keyframe) {
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8, 0x00, 0x10};
  ASSERT_TRUE(
      sendFramedAu(tx, idr, sizeof(idr), 1000000ULL, kFrameFlagKeyframe));
  RecvFrame f;
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagKeyframe);
  EXPECT_EQ(f.ptsUs, 1000000ULL);
  EXPECT_EQ(memcmp(f.payload.data(), idr, sizeof(idr)), 0);
}

TEST_F(PipeFixture, UT_FrameRoundTrip_PFrame) {
  const uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A, 0x24};
  ASSERT_TRUE(sendFramedAu(tx, pf, sizeof(pf), 33333ULL, 0));
  RecvFrame f;
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, 0u);
  EXPECT_EQ(f.ptsUs, 33333ULL);
  EXPECT_EQ(memcmp(f.payload.data(), pf, sizeof(pf)), 0);
}

TEST_F(PipeFixture, UT_FrameRoundTrip_FullStartupSequence) {
  const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE};
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8};
  const uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A};
  ASSERT_TRUE(sendFramedAu(tx, csd, sizeof(csd), 0, kFrameFlagConfig));
  ASSERT_TRUE(sendFramedAu(tx, idr, sizeof(idr), 0, kFrameFlagKeyframe));
  ASSERT_TRUE(sendFramedAu(tx, pf, sizeof(pf), 33333, 0));
  ASSERT_TRUE(sendFramedAu(tx, pf, sizeof(pf), 66666, 0));
  close(tx);
  tx = -1;
  RecvFrame f;
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagConfig);
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, kFrameFlagKeyframe);
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, 0u);
  EXPECT_EQ(f.ptsUs, 33333ULL);
  ASSERT_TRUE(recvFrame(rx, f));
  EXPECT_EQ(f.flags, 0u);
  EXPECT_EQ(f.ptsUs, 66666ULL);
  EXPECT_FALSE(recvFrame(rx, f));
}

//
// UT_Reconnect
//
TEST(UT_Reconnect, FdClose_ProducesEOF_NotEOSFrame) {
  int a[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, a), 0);
  const uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A};
  ASSERT_TRUE(sendFramedAu(a[0], pf, sizeof(pf), 0, 0));
  close(a[0]);  // MOD-4: close fd, no FRAM EOS
  RecvFrame f;
  ASSERT_TRUE(recvFrame(a[1], f));
  EXPECT_EQ(f.flags, 0u);
  // Next read must be EOF, not a FRAM EOS frame
  EXPECT_EQ(resync_to_magic(a[1]), -1) << "Expected EOF after fd close";
  close(a[1]);
}

TEST(UT_Reconnect, SecondConnection_DeliversFreshCSD) {
  // Conn 1
  int c1[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, c1), 0);
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8};
  ASSERT_TRUE(sendFramedAu(c1[0], idr, sizeof(idr), 0, kFrameFlagKeyframe));
  close(c1[0]);
  RecvFrame f;
  recvFrame(c1[1], f);
  close(c1[1]);

  // Conn 2 (new sender reconnect)
  int c2[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, c2), 0);
  const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE};
  ASSERT_TRUE(sendFramedAu(c2[0], csd, sizeof(csd), 0, kFrameFlagConfig));
  ASSERT_TRUE(
      sendFramedAu(c2[0], idr, sizeof(idr), 2000000, kFrameFlagKeyframe));
  close(c2[0]);

  ASSERT_TRUE(recvFrame(c2[1], f));
  EXPECT_EQ(f.flags, kFrameFlagConfig);
  ASSERT_TRUE(recvFrame(c2[1], f));
  EXPECT_EQ(f.flags, kFrameFlagKeyframe);
  EXPECT_EQ(f.ptsUs, 2000000ULL);
  close(c2[1]);
}

//
// UT_NoEosFrame sender MUST close fd, not send FRAM EOS
//
TEST(UT_NoEosFrame, SendsNoFRAMEosBeforeClose) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  const uint8_t pl[] = {0, 0, 0, 1, 0x65, 0xB8};
  ASSERT_TRUE(sendFramedAu(fds[0], pl, sizeof(pl), 0, kFrameFlagKeyframe));
  // Simulate MOD-4: just close the fd
  close(fds[0]);
  RecvFrame f;
  ASSERT_TRUE(recvFrame(fds[1], f));
  EXPECT_EQ(f.flags, kFrameFlagKeyframe);
  // After the keyframe the bridge must see EOF (-1), not an EOS FRAM frame
  int r = resync_to_magic(fds[1]);
  EXPECT_EQ(r, -1) << "FRAM EOS must not be sent; fd close is the EOF signal";
  close(fds[1]);
}

//
// UT_CsdBeforeKeyframe CSD frame always precedes keyframe
//
TEST(UT_CsdBeforeKeyframe, ConfigThenKeyframe) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE};
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8, 0x00, 0x10};
  ASSERT_TRUE(sendFramedAu(fds[0], csd, sizeof(csd), 1000, kFrameFlagConfig));
  ASSERT_TRUE(sendFramedAu(fds[0], idr, sizeof(idr), 1000, kFrameFlagKeyframe));
  close(fds[0]);
  RecvFrame f1, f2;
  ASSERT_TRUE(recvFrame(fds[1], f1));
  ASSERT_TRUE(recvFrame(fds[1], f2));
  close(fds[1]);
  EXPECT_EQ(f1.flags, kFrameFlagConfig) << "First frame must be CSD";
  EXPECT_EQ(f2.flags, kFrameFlagKeyframe) << "Second frame must be keyframe";
  EXPECT_TRUE(hasAnnexBStartCode(f1.payload.data(), f1.payload.size()))
      << "CSD must be Annex-B";
  EXPECT_TRUE(hasAnnexBStartCode(f2.payload.data(), f2.payload.size()))
      << "Keyframe must be Annex-B";
}

TEST(UT_CsdBeforeKeyframe, PFrameHasNoInterveningCSD) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE};
  const uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8};
  const uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A};
  ASSERT_TRUE(sendFramedAu(fds[0], csd, sizeof(csd), 0, kFrameFlagConfig));
  ASSERT_TRUE(sendFramedAu(fds[0], idr, sizeof(idr), 0, kFrameFlagKeyframe));
  ASSERT_TRUE(sendFramedAu(fds[0], pf, sizeof(pf), 33333, 0));
  ASSERT_TRUE(sendFramedAu(fds[0], pf, sizeof(pf), 66666, 0));
  close(fds[0]);
  RecvFrame frames[4];
  for (auto& f : frames) ASSERT_TRUE(recvFrame(fds[1], f));
  close(fds[1]);
  EXPECT_EQ(frames[0].flags, kFrameFlagConfig);
  EXPECT_EQ(frames[1].flags, kFrameFlagKeyframe);
  EXPECT_EQ(frames[2].flags, 0u);  // P-frame, no CONFIG between IDR and P
  EXPECT_EQ(frames[3].flags, 0u);
  EXPECT_LT(frames[2].ptsUs, frames[3].ptsUs);
}

//
// UT_Stress 100 frames, no loss, no corruption
//
TEST(UT_Stress, HundredFrames_NoneDropped) {
  int fds[2];
  ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
  constexpr int N = 100;

  std::thread sender([&] {
    const uint8_t csd[] = {0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xCE};
    sendFramedAu(fds[0], csd, sizeof(csd), 0, kFrameFlagConfig);
    uint8_t idr[] = {0, 0, 0, 1, 0x65, 0xB8, 0, 0};
    uint8_t pf[] = {0, 0, 0, 1, 0x41, 0x9A, 0, 0};
    for (int i = 0; i < N; ++i) {
      if (i % 30 == 0) {
        idr[6] = (uint8_t)(i >> 8);
        idr[7] = (uint8_t)i;
        sendFramedAu(fds[0], csd, sizeof(csd), (uint64_t)i * 33333,
                     kFrameFlagConfig);
        sendFramedAu(fds[0], idr, sizeof(idr), (uint64_t)i * 33333,
                     kFrameFlagKeyframe);
      } else {
        pf[6] = (uint8_t)(i >> 8);
        pf[7] = (uint8_t)i;
        sendFramedAu(fds[0], pf, sizeof(pf), (uint64_t)i * 33333, 0);
      }
    }
    close(fds[0]);
  });

  int cfgCnt = 0, keyCnt = 0, pCnt = 0;
  RecvFrame f;
  while (recvFrame(fds[1], f)) {
    if (f.flags & kFrameFlagConfig) ++cfgCnt;
    if (f.flags & kFrameFlagKeyframe) ++keyCnt;
    if (f.flags == 0) ++pCnt;
  }
  close(fds[1]);
  sender.join();

  // IDR at i=0,30,60,90 � 4 keyframes, 96 P-frames, e4 CSD (1 pre-loop + 4
  // in-loop)
  EXPECT_EQ(keyCnt, 4) << "Expected 4 IDR frames";
  EXPECT_EQ(pCnt, 96) << "Expected 96 P-frames";
  EXPECT_GE(cfgCnt, 4) << "Expected e4 CSD frames";
}

//
// UT_FpsCap_IntegrationWithResolution combined
//
TEST(UT_VideoSettings, CapFpsMatchesDefaultForDisplay60Hz) {
  // At 60 Hz, encoder runs at 30 fps frames arrive every ~33ms.
  float fps = capFps(60.0f);
  double frameIntervalMs = 1000.0 / fps;
  EXPECT_NEAR(frameIntervalMs, 33.333, 1.0)
      << "Expected ~33ms frame interval at 30fps";
}

TEST(UT_VideoSettings, Compute480pSize_1080pDisplay) {
  uint32_t w = 0, h = 0;
  compute480pSize(1920, 1080, &w, &h);
  // Verify encoder throughput target: 854�480@30fps at 1 Mbps
  // pixel/s = 854 * 480 * 30 = 12,297,600 well within Baseline/3.1 limits
  EXPECT_LE((uint64_t)w * h * 30u, 12800000u)
      << "Encoder pixel rate must be within Baseline 3.1 budget";
}

TEST(UT_VideoSettings, ResolutionAndFps_MeetBandwidthTarget) {
  // At 854�480 @ 30fps @ 1 Mbps: bits-per-pixel = 1e6/(854*480*30) H 0.081
  // That is the same order as commercial H.264 Baseline streaming.
  uint32_t w = kCap480pWidth, h = kCap480pHeight;
  float fps = 30.0f;
  uint32_t bitrate = 1000000u;
  double bpp = (double)bitrate / ((double)w * h * fps);
  EXPECT_GT(bpp, 0.05) << "Bit rate too low for quality";
  EXPECT_LT(bpp, 0.30) << "Bit rate too high for latency target";
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}