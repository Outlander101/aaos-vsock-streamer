
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fram {

// AGL flags
enum FramFlag : uint32_t {
  kNone = 0,
  kKeyframe = 0x01u,  // FLAG_KEYFRAME
  kEOS = 0x04u,       // FLAG_EOS
  kConfig = 0x08u,    // FLAG_CONFIG
};

enum class Endian { Little, Big };

struct Header {
  uint32_t len = 0;     // payload length
  uint32_t pts_hi = 0;  // upper 32-bits of PTS (from AGL pts_us u64)
  uint32_t pts_lo = 0;  // lower 32-bits of PTS
  uint32_t flags = 0;   // bitmask (AGL-compatible)
};

// Returns true if buf[0..3] == "FRAM"
bool is_magic_fram(const uint8_t magic[4]);

// Decode u32 from p with chosen endian
uint32_t rd_u32(const uint8_t* p, Endian e);

// Parse 16-byte header for the canonical AGL layout:
//   len:u32 @0, pts:u64 @4, flags:u32 @12
Header parse_header(const uint8_t hdr16[16], Endian e);

// Heuristic: choose LE/BE by picking a plausible length.
// If both plausible, prefer LE.
Endian detect_endian(const uint8_t hdr16[16], uint32_t max_len);

// Switch-case mapping single-bit flag to name.
// We map:
//   CONFIG -> "CSD"
//   KEYFRAME -> "IDR"
//   EOS -> "EOS"
std::string flag_name_single(uint32_t single_flag);

// Decode bitmask into names (CSD/IDR/EOS + unknown grouped)
std::vector<std::string> decode_flag_names(uint32_t flags);

// Convenience: "CSD|IDR" etc or "NONE"
std::string flags_to_string(uint32_t flags);

}  // namespace fram