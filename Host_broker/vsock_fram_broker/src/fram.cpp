
#include "fram.hpp"

#include <iomanip>
#include <sstream>

namespace fram {

bool is_magic_fram(const uint8_t magic[4]) {
  return magic[0] == 'F' && magic[1] == 'R' && magic[2] == 'A' &&
         magic[3] == 'M';
}

uint32_t rd_u32(const uint8_t* p, Endian e) {
  if (e == Endian::Little) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
  }
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t rd_u64(const uint8_t* p, Endian e) {
  if (e == Endian::Little) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
  }
  return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
         ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
         ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
         ((uint64_t)p[6] << 8) | (uint64_t)p[7];
}

// Canonical AGL layout: len@0, pts(u64)@4, flags@12
Header parse_header(const uint8_t hdr16[16], Endian e) {
  Header h{};
  h.len = rd_u32(hdr16 + 0, e);

  uint64_t pts = rd_u64(hdr16 + 4, e);
  h.pts_hi = (uint32_t)((pts >> 32) & 0xFFFFFFFFu);
  h.pts_lo = (uint32_t)(pts & 0xFFFFFFFFu);

  h.flags = rd_u32(hdr16 + 12, e);
  return h;
}

static bool plausible_len(uint32_t len, uint32_t max_len) {
  return len <= max_len;
}

Endian detect_endian(const uint8_t hdr16[16], uint32_t max_len) {
  Header le = parse_header(hdr16, Endian::Little);
  Header be = parse_header(hdr16, Endian::Big);

  bool le_ok = plausible_len(le.len, max_len);
  bool be_ok = plausible_len(be.len, max_len);

  if (le_ok && !be_ok) return Endian::Little;
  if (be_ok && !le_ok) return Endian::Big;
  if (le_ok && be_ok) return Endian::Little;
  return Endian::Little;
}

// ✅ Review comment: switch-case to map actual values
std::string flag_name_single(uint32_t single_flag) {
  switch (single_flag) {
    case (uint32_t)kConfig:
      return "CSD";  // FLAG_CONFIG
    case (uint32_t)kKeyframe:
      return "IDR";  // FLAG_KEYFRAME
    case (uint32_t)kEOS:
      return "EOS";  // FLAG_EOS
    case 0:
      return "NONE";
    default: {
      std::ostringstream oss;
      oss << "UNKNOWN(0x" << std::hex << single_flag << ")";
      return oss.str();
    }
  }
}

std::vector<std::string> decode_flag_names(uint32_t flags) {
  std::vector<std::string> out;

  if (flags == 0) {
    out.push_back("NONE");
    return out;
  }

  uint32_t known = 0;

  if (flags & (uint32_t)kConfig) {
    out.push_back(flag_name_single((uint32_t)kConfig));
    known |= (uint32_t)kConfig;
  }
  if (flags & (uint32_t)kKeyframe) {
    out.push_back(flag_name_single((uint32_t)kKeyframe));
    known |= (uint32_t)kKeyframe;
  }
  if (flags & (uint32_t)kEOS) {
    out.push_back(flag_name_single((uint32_t)kEOS));
    known |= (uint32_t)kEOS;
  }

  uint32_t unknown = flags & ~known;
  if (unknown) out.push_back(flag_name_single(unknown));

  return out;
}

std::string flags_to_string(uint32_t flags) {
  auto names = decode_flag_names(flags);
  if (names.empty()) return "NONE";

  std::ostringstream oss;
  for (size_t i = 0; i < names.size(); i++) {
    if (i) oss << "|";
    oss << names[i];
  }
  return oss.str();
}

}  // namespace fram