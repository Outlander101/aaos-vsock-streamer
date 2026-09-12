
// vsock-rtsp-fram-bridge.c
// Robust VSOCK FRAM(H264) -> RTSP bridge for AGL
//
// Design:
//  - RTSP server always up (boot-start).
//  - appsrc exists only when a client connects (media-configure).
//  - Push only when appsrc_ready=1.
//  - Cache SPS/PPS + last IDR; on client connect push CSD -> IDR -> live.
//  - Auto-detect FRAM header layout (BE/LE + field order) to match sender.
//  - Strong logging to debug.
//  - VSOCK MODE CHANGE: AGL CONNECTS to HOST (host broker LISTENS).
//
// Build deps: gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0
// glib-2.0
//
#include <errno.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <linux/vm_sockets.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#define MAX_FRAME_SIZE (8u * 1024u * 1024u)
#define FLAG_KEYFRAME 0x01u
#define FLAG_EOS 0x04u
#define FLAG_CONFIG 0x08u
static gchar *g_rtsp_service = NULL;  // "8554"
static gchar *g_rtsp_mount = NULL;    // "/test"
static gboolean g_verbose = FALSE;
/* NEW: connect-to-host params */
static guint g_host_cid = 2;      // VMADDR_CID_HOST for Android guest
static guint g_host_port = 5000;  // host broker listens here for AGL uplink
/* appsrc shared pointer (valid only when RTSP client connected) */
static GstElement *g_appsrc = NULL;
static gboolean g_appsrc_ready = FALSE;
static GMutex g_appsrc_lock;
/* Cached SPS/PPS and last IDR (AnnexB) */
static GMutex g_cache_lock;
static uint8_t *g_cached_csd = NULL;
static uint32_t g_cached_csd_len = 0;
static uint8_t *g_cached_idr = NULL;
static uint32_t g_cached_idr_len = 0;
/* ----------------- endian helpers ----------------- */
static inline uint32_t rd_u32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline uint64_t rd_u64_le(const uint8_t *p) {
  return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}
static inline uint32_t rd_u32_be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static inline uint64_t rd_u64_be(const uint8_t *p) {
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}
static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
}
static ssize_t read_full(int fd, void *buf, size_t n) {
  uint8_t *p = (uint8_t *)buf;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);
    if (r == 0) return 0;
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    got += (size_t)r;
  }
  return (ssize_t)got;
}
/* ---- FRAM resync ---- */
static int resync_to_magic(int fd) {
  uint8_t w[4];
  if (read_full(fd, w, 4) <= 0) return -1;
  for (;;) {
    if (w[0] == 'F' && w[1] == 'R' && w[2] == 'A' && w[3] == 'M') return 0;
    w[0] = w[1];
    w[1] = w[2];
    w[2] = w[3];
    ssize_t r = read(fd, &w[3], 1);
    if (r <= 0) return -1;
  }
}
/* ---- VSOCK connect (NEW) ---- */
static int vsock_connect(uint32_t cid, uint32_t port) {
  int s = socket(AF_VSOCK, SOCK_STREAM, 0);
  if (s < 0) {
    perror("socket(AF_VSOCK)");
    return -1;
  }
  struct sockaddr_vm sa;
  memset(&sa, 0, sizeof(sa));
  sa.svm_family = AF_VSOCK;
  sa.svm_cid = cid;
  sa.svm_port = port;
  if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(s);
    return -1;
  }
  return s;
}
/* ---- AnnexB vs AVCC ---- */
static gboolean looks_like_annexb(const uint8_t *p, uint32_t len) {
  if (!p || len < 4) return FALSE;
  if (p[0] == 0 && p[1] == 0 && p[2] == 1) return TRUE;
  if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) return TRUE;
  return FALSE;
}
/* Convert AVCC frame with 4-byte big-endian lengths to AnnexB */
static uint8_t *avcc_to_annexb(const uint8_t *src, uint32_t in_len,
                               uint32_t *out_len) {
  if (!src || in_len < 4) return NULL;
  uint32_t pos = 0;
  uint64_t total = 0;
  while (pos + 4 <= in_len) {
    uint32_t n = (src[pos] << 24) | (src[pos + 1] << 16) | (src[pos + 2] << 8) |
                 src[pos + 3];
    pos += 4;
    if (n == 0 || pos + n > in_len) break;
    total += 4 + n;
    pos += n;
  }
  if (total == 0 || total > (uint64_t)MAX_FRAME_SIZE * 2) return NULL;
  uint8_t *dst = (uint8_t *)malloc((size_t)total);
  if (!dst) return NULL;
  static const uint8_t sc4[4] = {0, 0, 0, 1};
  pos = 0;
  uint32_t w = 0;
  while (pos + 4 <= in_len) {
    uint32_t n = (src[pos] << 24) | (src[pos + 1] << 16) | (src[pos + 2] << 8) |
                 src[pos + 3];
    pos += 4;
    if (n == 0 || pos + n > in_len) break;
    memcpy(dst + w, sc4, 4);
    w += 4;
    memcpy(dst + w, src + pos, n);
    w += n;
    pos += n;
  }
  *out_len = w;
  return dst;
}
static void cache_replace(uint8_t **dst, uint32_t *dst_len, const uint8_t *src,
                          uint32_t src_len) {
  free(*dst);
  *dst = NULL;
  *dst_len = 0;
  if (!src || src_len == 0) return;
  *dst = (uint8_t *)malloc(src_len);
  if (!*dst) return;
  memcpy(*dst, src, src_len);
  *dst_len = src_len;
}
static gboolean annexb_contains_idr(const uint8_t *p, uint32_t len) {
  if (!p || len < 5) return FALSE;
  for (uint32_t i = 0; i + 5 < len; i++) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
      uint8_t nal_type = p[i + 3] & 0x1F;
      if (nal_type == 5) return TRUE;
    }
    if (i + 6 < len && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 &&
        p[i + 3] == 1) {
      uint8_t nal_type = p[i + 4] & 0x1F;
      if (nal_type == 5) return TRUE;
    }
  }
  return FALSE;
}
/* Extract SPS/PPS from AnnexB payload and cache concatenated SPS+PPS */
static void extract_and_cache_csd_from_annexb(const uint8_t *payload,
                                              uint32_t len) {
  if (!payload || len < 6) return;
  const uint8_t *sps_ptr = NULL, *pps_ptr = NULL;
  size_t sps_len = 0, pps_len = 0;
#define HAS_SC3(p) ((p)[0] == 0 && (p)[1] == 0 && (p)[2] == 1)
#define HAS_SC4(p) ((p)[0] == 0 && (p)[1] == 0 && (p)[2] == 0 && (p)[3] == 1)
  size_t i = 0;
  while (i + 3 < len) {
    size_t sc = (size_t)-1, sc_len = 0;
    while (i + 3 < len) {
      if (HAS_SC3(payload + i)) {
        sc = i;
        sc_len = 3;
        break;
      }
      if (i + 4 < len && HAS_SC4(payload + i)) {
        sc = i;
        sc_len = 4;
        break;
      }
      ++i;
    }
    if (sc == (size_t)-1) break;
    size_t nal_start = sc + sc_len;
    if (nal_start >= len) break;
    uint8_t nal_type = payload[nal_start] & 0x1F;
    size_t j = nal_start + 1;
    size_t next_sc = len;
    while (j + 3 < len) {
      if (HAS_SC3(payload + j) || (j + 4 < len && HAS_SC4(payload + j))) {
        next_sc = j;
        break;
      }
      ++j;
    }
    if (nal_type == 7) {
      sps_ptr = payload + sc;
      sps_len = next_sc - sc;
    }
    if (nal_type == 8) {
      pps_ptr = payload + sc;
      pps_len = next_sc - sc;
    }
    if (sps_ptr && pps_ptr) {
      uint32_t total = (uint32_t)(sps_len + pps_len);
      uint8_t *tmp = (uint8_t *)malloc(total);
      if (tmp) {
        memcpy(tmp, sps_ptr, sps_len);
        memcpy(tmp + sps_len, pps_ptr, pps_len);
        g_mutex_lock(&g_cache_lock);
        cache_replace(&g_cached_csd, &g_cached_csd_len, tmp, total);
        g_mutex_unlock(&g_cache_lock);
        free(tmp);
        if (g_verbose)
          g_print("[bridge][INFO] cached SPS/PPS from stream (len=%u)\n",
                  total);
      }
      return;
    }
    i = next_sc;
  }
#undef HAS_SC3
#undef HAS_SC4
}
/* ---- appsrc helpers ---- */
static gboolean appsrc_get(GstElement **out) {
  *out = NULL;
  g_mutex_lock(&g_appsrc_lock);
  if (g_appsrc && g_appsrc_ready) *out = GST_ELEMENT(gst_object_ref(g_appsrc));
  g_mutex_unlock(&g_appsrc_lock);
  return (*out != NULL);
}
static void appsrc_set_ready(GstElement *src, gboolean ready, const char *why) {
  g_mutex_lock(&g_appsrc_lock);
  if (!ready) {
    g_appsrc_ready = FALSE;
    if (g_appsrc) {
      gst_object_unref(g_appsrc);
      g_appsrc = NULL;
    }
  } else {
    if (g_appsrc) gst_object_unref(g_appsrc);
    g_appsrc = (GstElement *)gst_object_ref(src);
    g_appsrc_ready = TRUE;
  }
  g_mutex_unlock(&g_appsrc_lock);
  g_print("[bridge][INFO] appsrc_ready=%d (%s)\n", ready ? 1 : 0,
          why ? why : "");
}
static void push_bytes_to_appsrc(const uint8_t *payload, uint32_t len,
                                 uint32_t flags) {
  GstElement *src = NULL;
  if (!appsrc_get(&src)) {
    if (g_verbose && (len > 0 || (flags & FLAG_EOS)))
      g_print(
          "[bridge][DBG] drop: no RTSP client/appsrc yet (len=%u flags=0x%x)\n",
          len, flags);
    return;
  }
  if (flags & FLAG_EOS) {
    gst_app_src_end_of_stream(GST_APP_SRC(src));
    gst_object_unref(src);
    return;
  }
  if (!payload || len == 0) {
    gst_object_unref(src);
    return;
  }
  GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
  if (!buf) {
    gst_object_unref(src);
    return;
  }
  gst_buffer_fill(buf, 0, payload, len);
  GST_BUFFER_PTS(buf) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS(buf) = GST_CLOCK_TIME_NONE;
  if (flags & FLAG_KEYFRAME)
    GST_BUFFER_FLAG_UNSET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
  else
    GST_BUFFER_FLAG_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);
  GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(src), buf);
  if (ret != GST_FLOW_OK) {
    g_printerr("[bridge][WARN] appsrc push failed: %d\n", ret);
  }
  gst_object_unref(src);
}
/* ---- FRAM header auto-detect ---- */
typedef enum {
  FRAM_FMT_LE_LEN_PTS_FLAGS = 0,
  FRAM_FMT_BE_LEN_PTS_FLAGS = 1,
  FRAM_FMT_LE_FLAGS_LEN_PTS = 2,
  FRAM_FMT_BE_FLAGS_LEN_PTS = 3,
} FramFmt;
static inline gboolean flags_sane(uint32_t f) {
  const uint32_t allowed = (FLAG_KEYFRAME | FLAG_EOS | FLAG_CONFIG);
  return (f & ~allowed) == 0;
}
static inline gboolean try_decode_hdr16(FramFmt fmt, const uint8_t hdr[16],
                                        uint32_t *len, uint64_t *pts_us,
                                        uint32_t *flags) {
  uint32_t l = 0, f = 0;
  uint64_t pts = 0;
  switch (fmt) {
    case FRAM_FMT_LE_LEN_PTS_FLAGS:
      l = rd_u32_le(hdr + 0);
      pts = rd_u64_le(hdr + 4);
      f = rd_u32_le(hdr + 12);
      break;
    case FRAM_FMT_BE_LEN_PTS_FLAGS:
      l = rd_u32_be(hdr + 0);
      pts = rd_u64_be(hdr + 4);
      f = rd_u32_be(hdr + 12);
      break;
    case FRAM_FMT_LE_FLAGS_LEN_PTS:
      f = rd_u32_le(hdr + 0);
      l = rd_u32_le(hdr + 4);
      pts = rd_u64_le(hdr + 8);
      break;
    case FRAM_FMT_BE_FLAGS_LEN_PTS:
      f = rd_u32_be(hdr + 0);
      l = rd_u32_be(hdr + 4);
      pts = rd_u64_be(hdr + 8);
      break;
  }
  if (l > MAX_FRAME_SIZE) return FALSE;
  if (!flags_sane(f)) return FALSE;
  if (l == 0 && !(f & FLAG_EOS) && !(f & FLAG_CONFIG)) return FALSE;
  *len = l;
  *pts_us = pts;
  *flags = f;
  return TRUE;
}
static const char *fmt_name(FramFmt f) {
  switch (f) {
    case FRAM_FMT_LE_LEN_PTS_FLAGS:
      return "LE len,pts,flags";
    case FRAM_FMT_BE_LEN_PTS_FLAGS:
      return "BE len,pts,flags";
    case FRAM_FMT_LE_FLAGS_LEN_PTS:
      return "LE flags,len,pts";
    case FRAM_FMT_BE_FLAGS_LEN_PTS:
      return "BE flags,len,pts";
  }
  return "unknown";
}
static void dump_hdr16(const uint8_t hdr[16]) {
  g_printerr(
      "[bridge][DBG] hdr16: "
      "%02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x "
      "%02x %02x %02x\n",
      hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6], hdr[7], hdr[8],
      hdr[9], hdr[10], hdr[11], hdr[12], hdr[13], hdr[14], hdr[15]);
}
/* ---- RTSP callbacks ---- */
static void media_unprepared_cb(GstRTSPMedia *media, gpointer user_data) {
  (void)media;
  (void)user_data;
  appsrc_set_ready(NULL, FALSE, "media-unprepared (client gone)");
}
static void media_configure_cb(GstRTSPMediaFactory *factory,
                               GstRTSPMedia *media, gpointer user_data) {
  (void)factory;
  (void)user_data;
  g_print("[bridge][INFO] media-configure fired\n");
  gst_rtsp_media_set_stop_on_disconnect(media, FALSE);
  g_signal_connect(media, "unprepared", (GCallback)media_unprepared_cb, NULL);
  GstElement *pipe = gst_rtsp_media_get_element(media);
  if (!pipe) {
    g_printerr("[bridge][ERROR] no media element\n");
    return;
  }
  GstElement *src = gst_bin_get_by_name_recurse_up(GST_BIN(pipe), "src");
  if (!src) {
    g_printerr("[bridge][ERROR] appsrc 'src' not found in pipeline\n");
    gst_object_unref(pipe);
    return;
  }
  g_object_set(G_OBJECT(src), "is-live", TRUE, "format", GST_FORMAT_TIME,
               "block", FALSE, "do-timestamp", TRUE, NULL);
  appsrc_set_ready(src, TRUE, "media-configure (client connected)");
  // Bootstrap: SPS/PPS then IDR
  g_mutex_lock(&g_cache_lock);
  if (g_cached_csd && g_cached_csd_len > 0) {
    g_print("[bridge][INFO] bootstrap: push SPS/PPS (%u bytes)\n",
            g_cached_csd_len);
    push_bytes_to_appsrc(g_cached_csd, g_cached_csd_len, FLAG_CONFIG);
  } else {
    g_print("[bridge][INFO] bootstrap: no SPS/PPS cached yet\n");
  }
  if (g_cached_idr && g_cached_idr_len > 0) {
    g_print("[bridge][INFO] bootstrap: push IDR (%u bytes)\n",
            g_cached_idr_len);
    push_bytes_to_appsrc(g_cached_idr, g_cached_idr_len, FLAG_KEYFRAME);
  } else {
    g_print("[bridge][INFO] bootstrap: no IDR cached yet\n");
  }
  g_mutex_unlock(&g_cache_lock);
  gst_object_unref(src);
  gst_object_unref(pipe);
}
/* ---- Startup checks: plugin presence ---- */
static void require_element_or_die(const char *name) {
  GstElementFactory *f = gst_element_factory_find(name);
  if (!f) {
    g_printerr("[bridge][FATAL] Missing GStreamer element '%s'.\n", name);
    exit(2);
  }
  gst_object_unref(f);
}
/* ---- VSOCK thread (NEW: connect loop) ---- */
static gpointer vsock_thread(gpointer user_data) {
  (void)user_data;
  for (;;) {
    g_print("[bridge][INFO] connecting to host via vsock cid=%u port=%u ...\n",
            g_host_cid, g_host_port);
    int c = -1;
    while (c < 0) {
      c = vsock_connect(g_host_cid, g_host_port);
      if (c < 0) {
        if (g_verbose)
          g_printerr("[bridge][WARN] host connect failed (%s). retry in 1s\n",
                     strerror(errno));
        sleep(1);
      }
    }
    g_print("[bridge][INFO] connected to host (fd=%d). reading FRAM stream\n",
            c);
    uint64_t frames = 0, bytes = 0, keyframes = 0, configs = 0, drops = 0;
    uint64_t last_stat = now_ms();
    gboolean fmt_locked = FALSE;
    FramFmt locked_fmt = FRAM_FMT_BE_LEN_PTS_FLAGS;
    const FramFmt candidates[] = {
        FRAM_FMT_BE_LEN_PTS_FLAGS,
        FRAM_FMT_LE_LEN_PTS_FLAGS,
        FRAM_FMT_BE_FLAGS_LEN_PTS,
        FRAM_FMT_LE_FLAGS_LEN_PTS,
    };
    for (;;) {
      uint8_t magic[4];
      ssize_t mr = read_full(c, magic, 4);
      if (mr == 0) {
        g_print("[bridge][INFO] EOF from host\n");
        break;
      }
      if (mr < 0) {
        perror("[bridge] read magic");
        break;
      }
      if (!(magic[0] == 'F' && magic[1] == 'R' && magic[2] == 'A' &&
            magic[3] == 'M')) {
        g_printerr("[bridge][WARN] bad magic; resyncing...\n");
        if (resync_to_magic(c) != 0) break;
      }
      uint8_t hdr[16];
      ssize_t hr = read_full(c, hdr, sizeof(hdr));
      if (hr == 0) {
        g_print("[bridge][INFO] EOF from host\n");
        break;
      }
      if (hr < 0) {
        perror("[bridge] read header");
        break;
      }
      uint32_t length = 0, flags = 0;
      uint64_t pts_us = 0;
      if (!fmt_locked) {
        gboolean ok = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS(candidates); i++) {
          if (try_decode_hdr16(candidates[i], hdr, &length, &pts_us, &flags)) {
            locked_fmt = candidates[i];
            fmt_locked = TRUE;
            ok = TRUE;
            g_print(
                "[bridge][INFO] FRAM format locked: %s (len=%u flags=0x%x)\n",
                fmt_name(locked_fmt), length, flags);
            break;
          }
        }
        if (!ok) {
          g_printerr(
              "[bridge][ERROR] could not decode FRAM header; resyncing\n");
          dump_hdr16(hdr);
          if (resync_to_magic(c) != 0) break;
          drops++;
          continue;
        }
      } else {
        if (!try_decode_hdr16(locked_fmt, hdr, &length, &pts_us, &flags)) {
          g_printerr(
              "[bridge][WARN] header invalid for locked format (%s); "
              "resyncing\n",
              fmt_name(locked_fmt));
          dump_hdr16(hdr);
          if (resync_to_magic(c) != 0) break;
          drops++;
          continue;
        }
      }
      uint8_t *payload = NULL;
      if (length > 0) {
        payload = (uint8_t *)malloc(length);
        if (!payload) {
          g_printerr("[bridge][ERROR] OOM length=%u\n", length);
          break;
        }
        ssize_t pr = read_full(c, payload, length);
        if (pr == 0) {
          free(payload);
          g_print("[bridge][INFO] EOF from host\n");
          break;
        }
        if (pr < 0) {
          free(payload);
          perror("[bridge] read payload");
          break;
        }
      }
      frames++;
      bytes += length;
      if (flags & FLAG_KEYFRAME) keyframes++;
      if (flags & FLAG_CONFIG) configs++;
      uint8_t *to_push = payload;
      uint32_t to_len = length;
      uint8_t *converted = NULL;
      if (to_push && to_len > 0 && !looks_like_annexb(to_push, to_len)) {
        uint32_t out_len = 0;
        converted = avcc_to_annexb(to_push, to_len, &out_len);
        if (converted && out_len > 0) {
          to_push = converted;
          to_len = out_len;
          if (g_verbose)
            g_print("[bridge][INFO] AVCC->AnnexB %u -> %u\n", length, out_len);
        } else {
          g_printerr(
              "[bridge][WARN] not AnnexB and AVCC convert failed; dropping "
              "frame len=%u\n",
              length);
          free(payload);
          free(converted);
          drops++;
          continue;
        }
      }
      if (to_push && to_len > 0) {
        // Cache SPS/PPS + IDR
        if (flags & FLAG_CONFIG) {
          g_mutex_lock(&g_cache_lock);
          cache_replace(&g_cached_csd, &g_cached_csd_len, to_push, to_len);
          g_mutex_unlock(&g_cache_lock);
          if (g_verbose)
            g_print("[bridge][INFO] cached CSD via FLAG_CONFIG len=%u\n",
                    to_len);
        } else {
          extract_and_cache_csd_from_annexb(to_push, to_len);
        }
        if ((flags & FLAG_KEYFRAME) || annexb_contains_idr(to_push, to_len)) {
          g_mutex_lock(&g_cache_lock);
          cache_replace(&g_cached_idr, &g_cached_idr_len, to_push, to_len);
          g_mutex_unlock(&g_cache_lock);
          if (g_verbose) g_print("[bridge][INFO] cached IDR len=%u\n", to_len);
        }
        // Push only if RTSP client has created appsrc
        push_bytes_to_appsrc(to_push, to_len, flags);
      } else if (flags & FLAG_EOS) {
        push_bytes_to_appsrc(NULL, 0, flags);
      }
      free(payload);
      free(converted);
      uint64_t t = now_ms();
      if (t - last_stat >= 2000) {
        double mb = (double)bytes / (1024.0 * 1024.0);
        gboolean ready;
        g_mutex_lock(&g_appsrc_lock);
        ready = g_appsrc_ready;
        g_mutex_unlock(&g_appsrc_lock);
        g_print("[bridge][STAT] frames=%" G_GUINT64_FORMAT
                " key=%" G_GUINT64_FORMAT " cfg=%" G_GUINT64_FORMAT
                " drop=%" G_GUINT64_FORMAT " MB=%.2f appsrc_ready=%d\n",
                (guint64)frames, (guint64)keyframes, (guint64)configs,
                (guint64)drops, mb, ready ? 1 : 0);
        last_stat = t;
      }
    }
    close(c);
    g_print("[bridge][INFO] host connection lost; retry connect\n");
    sleep(1);
  }
  return NULL;
}
int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  g_mutex_init(&g_appsrc_lock);
  g_mutex_init(&g_cache_lock);
  g_rtsp_service = g_strdup("8554");
  g_rtsp_mount = g_strdup("/test");
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--rtsp-port") && i + 1 < argc) {
      g_free(g_rtsp_service);
      g_rtsp_service = g_strdup(argv[++i]);
    } else if (!strcmp(argv[i], "--mount") && i + 1 < argc) {
      g_free(g_rtsp_mount);
      g_rtsp_mount = g_strdup(argv[++i]);
    } else if (!strcmp(argv[i], "--verbose"))
      g_verbose = TRUE;
    else if (!strcmp(argv[i], "--host-cid") && i + 1 < argc)
      g_host_cid = (guint)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--host-port") && i + 1 < argc)
      g_host_port = (guint)atoi(argv[++i]);
  }
  // Hard fail early if plugins are missing (prevents SDP confusion)
  require_element_or_die("h264parse");
  require_element_or_die("rtph264pay");
  const gchar *launch =
      "( appsrc name=src is-live=true format=time block=false "
      "do-timestamp=true "
      "   "
      "caps=video/"
      "x-h264,stream-format=(string)byte-stream,alignment=(string)au "
      " ! queue leaky=downstream max-size-buffers=0 max-size-bytes=0 "
      "max-size-time=0 "
      " ! h264parse config-interval=-1 "
      " ! rtph264pay name=pay0 pt=96 config-interval=1 )";
  // Validate launch at startup
  {
    GError *err = NULL;
    GstElement *test = gst_parse_launch(launch, &err);
    if (!test) {
      g_printerr("[bridge][FATAL] gst_parse_launch failed: %s\n",
                 err ? err->message : "unknown");
      if (err) g_error_free(err);
      return 2;
    }
    gst_object_unref(test);
  }
  GstRTSPServer *server = gst_rtsp_server_new();
  gst_rtsp_server_set_service(server, g_rtsp_service);
  GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);
  GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();
  gst_rtsp_media_factory_set_launch(factory, launch);
  gst_rtsp_media_factory_set_shared(factory, TRUE);
  gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_NONE);
  g_signal_connect(factory, "media-configure", (GCallback)media_configure_cb,
                   NULL);
  gst_rtsp_mount_points_add_factory(mounts, g_rtsp_mount, factory);
  g_object_unref(mounts);
  if (gst_rtsp_server_attach(server, NULL) == 0) {
    g_printerr("[bridge][FATAL] Failed to attach RTSP server\n");
    return 1;
  }
  g_print("[bridge][INFO] RTSP ready: rtsp://127.0.0.1:%s%s\n", g_rtsp_service,
          g_rtsp_mount);
  g_print("[bridge][INFO] VSOCK mode: connect to host cid=%u port=%u\n",
          g_host_cid, g_host_port);
  g_print(
      "[bridge][INFO] Note: appsrc_ready stays 0 until a client connects "
      "(QML/gst-launch).\n");
  g_thread_new("vsock-reader", vsock_thread, NULL);
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  return 0;
}