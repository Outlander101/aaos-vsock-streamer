
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define main bridge_production_main
#include "../vsock-rtsp-fram-bridge.c"
#undef main

// -------- tiny test harness --------
#define RUN_TEST(fn)               \
  do {                             \
    printf("[TEST] %s...\n", #fn); \
    fn();                          \
    printf("[PASS] %s\n", #fn);    \
  } while (0)

static void wr_u32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xff);
  p[1] = (uint8_t)((v >> 8) & 0xff);
  p[2] = (uint8_t)((v >> 16) & 0xff);
  p[3] = (uint8_t)((v >> 24) & 0xff);
}

static void wr_u32_be(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xff);
  p[1] = (uint8_t)((v >> 16) & 0xff);
  p[2] = (uint8_t)((v >> 8) & 0xff);
  p[3] = (uint8_t)(v & 0xff);
}

static void wr_u64_le(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)((v >> (8 * i)) & 0xff);
  }
}

static void wr_u64_be(uint8_t *p, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)((v >> (56 - 8 * i)) & 0xff);
  }
}

static void reset_caches(void) {
  g_mutex_lock(&g_cache_lock);
  free(g_cached_csd);
  g_cached_csd = NULL;
  g_cached_csd_len = 0;
  free(g_cached_idr);
  g_cached_idr = NULL;
  g_cached_idr_len = 0;
  g_mutex_unlock(&g_cache_lock);
}

static void test_looks_like_annexb(void) {
  const uint8_t sc3[] = {0x00, 0x00, 0x01, 0x67, 0x11};
  const uint8_t sc4[] = {0x00, 0x00, 0x00, 0x01, 0x68, 0x22};
  const uint8_t no[] = {0x12, 0x34, 0x56, 0x78, 0x9a};

  assert(looks_like_annexb(sc3, sizeof(sc3)) == TRUE);
  assert(looks_like_annexb(sc4, sizeof(sc4)) == TRUE);
  assert(looks_like_annexb(no, sizeof(no)) == FALSE);
  assert(looks_like_annexb(NULL, 0) == FALSE);
}

static void test_avcc_to_annexb_basic(void) {
  // AVCC format: [lenBE][NAL][lenBE][NAL]
  // NAL1: 0x67 0xAA 0xBB (SPS-ish)
  // NAL2: 0x68 0xCC (PPS-ish)
  uint8_t avcc[4 + 3 + 4 + 2];
  // len=3
  avcc[0] = 0;
  avcc[1] = 0;
  avcc[2] = 0;
  avcc[3] = 3;
  avcc[4] = 0x67;
  avcc[5] = 0xAA;
  avcc[6] = 0xBB;
  // len=2
  avcc[7] = 0;
  avcc[8] = 0;
  avcc[9] = 0;
  avcc[10] = 2;
  avcc[11] = 0x68;
  avcc[12] = 0xCC;

  uint32_t out_len = 0;
  uint8_t *annexb = avcc_to_annexb(avcc, (uint32_t)sizeof(avcc), &out_len);
  assert(annexb != NULL);
  assert(out_len == (4 + 3) + (4 + 2));  // startcode+NAL + startcode+NAL

  // Must start with 00 00 00 01 then 0x67...
  assert(annexb[0] == 0 && annexb[1] == 0 && annexb[2] == 0 && annexb[3] == 1);
  assert(annexb[4] == 0x67);

  // Second NAL startcode should appear at offset 7 (4+3)
  assert(annexb[7] == 0 && annexb[8] == 0 && annexb[9] == 0 && annexb[10] == 1);
  assert(annexb[11] == 0x68);

  free(annexb);
}

static void test_try_decode_hdr16_all_formats(void) {
  // We'll craft a header with:
  // length = 100
  // pts_us = 0x1122334455667788
  // flags  = FLAG_KEYFRAME | FLAG_CONFIG (0x01 | 0x08 = 0x09)
  const uint32_t length = 100;
  const uint64_t pts = 0x1122334455667788ULL;
  const uint32_t flags = (FLAG_KEYFRAME | FLAG_CONFIG);

  uint8_t hdr[16];

  uint32_t out_len = 0, out_flags = 0;
  uint64_t out_pts = 0;

  // 1) LE len,pts,flags
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, length);
  wr_u64_le(hdr + 4, pts);
  wr_u32_le(hdr + 12, flags);
  assert(try_decode_hdr16(FRAM_FMT_LE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == TRUE);
  assert(out_len == length && out_pts == pts && out_flags == flags);

  // 2) BE len,pts,flags
  memset(hdr, 0, sizeof(hdr));
  wr_u32_be(hdr + 0, length);
  wr_u64_be(hdr + 4, pts);
  wr_u32_be(hdr + 12, flags);
  assert(try_decode_hdr16(FRAM_FMT_BE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == TRUE);

  // 3) LE flags,len,pts
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, flags);
  wr_u32_le(hdr + 4, length);
  wr_u64_le(hdr + 8, pts);
  assert(try_decode_hdr16(FRAM_FMT_LE_FLAGS_LEN_PTS, hdr, &out_len, &out_pts,
                          &out_flags) == TRUE);

  // 4) BE flags,len,pts
  memset(hdr, 0, sizeof(hdr));
  wr_u32_be(hdr + 0, flags);
  wr_u32_be(hdr + 4, length);
  wr_u64_be(hdr + 8, pts);
  assert(try_decode_hdr16(FRAM_FMT_BE_FLAGS_LEN_PTS, hdr, &out_len, &out_pts,
                          &out_flags) == TRUE);

  // Invalid flags should fail
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, length);
  wr_u64_le(hdr + 4, pts);
  wr_u32_le(hdr + 12, 0xFFFFFFFFu);
  assert(try_decode_hdr16(FRAM_FMT_LE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == FALSE);

  // Length too big should fail
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, MAX_FRAME_SIZE + 1);
  wr_u64_le(hdr + 4, pts);
  wr_u32_le(hdr + 12, flags);
  assert(try_decode_hdr16(FRAM_FMT_LE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == FALSE);

  // length==0 with no EOS/CONFIG should fail
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, 0);
  wr_u64_le(hdr + 4, pts);
  wr_u32_le(hdr + 12, FLAG_KEYFRAME);  // not EOS, not CONFIG
  assert(try_decode_hdr16(FRAM_FMT_LE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == FALSE);

  // length==0 with EOS should pass
  memset(hdr, 0, sizeof(hdr));
  wr_u32_le(hdr + 0, 0);
  wr_u64_le(hdr + 4, pts);
  wr_u32_le(hdr + 12, FLAG_EOS);
  assert(try_decode_hdr16(FRAM_FMT_LE_LEN_PTS_FLAGS, hdr, &out_len, &out_pts,
                          &out_flags) == TRUE);
}

static void test_annexb_contains_idr(void) {
  // AnnexB startcode + IDR NAL (type 5 => 0x65)
  const uint8_t payload[] = {0, 0, 0, 1, 0x65, 0x88, 0x99, 0xAA};
  assert(annexb_contains_idr(payload, (uint32_t)sizeof(payload)) == TRUE);

  // Non-IDR slice (type 1 => 0x61)
  const uint8_t nonidr[] = {0, 0, 0, 1, 0x61, 0x11, 0x22};
  assert(annexb_contains_idr(nonidr, (uint32_t)sizeof(nonidr)) == FALSE);
}

static void test_extract_and_cache_csd_from_annexb(void) {
  // Build AnnexB containing:
  // 00 00 00 01 67 ... (SPS)
  // 00 00 00 01 68 ... (PPS)
  const uint8_t sps[] = {0, 0, 0, 1, 0x67, 0x42, 0x00, 0x1E};
  const uint8_t pps[] = {0, 0, 0, 1, 0x68, 0xCE, 0x06, 0xE2};

  uint8_t buf[sizeof(sps) + sizeof(pps)];
  memcpy(buf, sps, sizeof(sps));
  memcpy(buf + sizeof(sps), pps, sizeof(pps));

  reset_caches();
  extract_and_cache_csd_from_annexb(buf, (uint32_t)sizeof(buf));

  g_mutex_lock(&g_cache_lock);
  assert(g_cached_csd != NULL);
  assert(g_cached_csd_len == (uint32_t)(sizeof(sps) + sizeof(pps)));
  assert(memcmp(g_cached_csd, buf, sizeof(buf)) == 0);
  g_mutex_unlock(&g_cache_lock);

  reset_caches();
}

int main(void) {
  // Minimal init for cache mutex (since we included the production file)
  g_mutex_init(&g_cache_lock);

  // Keep verbose off for clean test output
  g_verbose = FALSE;

  RUN_TEST(test_looks_like_annexb);
  RUN_TEST(test_avcc_to_annexb_basic);
  RUN_TEST(test_try_decode_hdr16_all_formats);
  RUN_TEST(test_annexb_contains_idr);
  RUN_TEST(test_extract_and_cache_csd_from_annexb);

  printf("\nAll unit tests passed.\n");
  return 0;
}