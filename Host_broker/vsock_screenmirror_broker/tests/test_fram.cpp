
#include <cstring>
#include <test_harness.hpp>

#include "fram.hpp"

static void put_u32_le(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
}
static void put_u32_be(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)((v >> 24) & 0xFF);
  p[1] = (uint8_t)((v >> 16) & 0xFF);
  p[2] = (uint8_t)((v >> 8) & 0xFF);
  p[3] = (uint8_t)(v & 0xFF);
}

static void put_u64_le(uint8_t* p, uint64_t v) {
  p[0] = (uint8_t)(v & 0xFF);
  p[1] = (uint8_t)((v >> 8) & 0xFF);
  p[2] = (uint8_t)((v >> 16) & 0xFF);
  p[3] = (uint8_t)((v >> 24) & 0xFF);
  p[4] = (uint8_t)((v >> 32) & 0xFF);
  p[5] = (uint8_t)((v >> 40) & 0xFF);
  p[6] = (uint8_t)((v >> 48) & 0xFF);
  p[7] = (uint8_t)((v >> 56) & 0xFF);
}
static void put_u64_be(uint8_t* p, uint64_t v) {
  p[0] = (uint8_t)((v >> 56) & 0xFF);
  p[1] = (uint8_t)((v >> 48) & 0xFF);
  p[2] = (uint8_t)((v >> 40) & 0xFF);
  p[3] = (uint8_t)((v >> 32) & 0xFF);
  p[4] = (uint8_t)((v >> 24) & 0xFF);
  p[5] = (uint8_t)((v >> 16) & 0xFF);
  p[6] = (uint8_t)((v >> 8) & 0xFF);
  p[7] = (uint8_t)(v & 0xFF);
}

TEST(fram_magic_valid) {
  uint8_t m[4] = {'F', 'R', 'A', 'M'};
  ASSERT_TRUE(fram::is_magic_fram(m));
}

TEST(fram_magic_invalid) {
  uint8_t m[4] = {'B', 'A', 'D', '!'};
  ASSERT_FALSE(fram::is_magic_fram(m));
}

TEST(rd_u32_le_basic) {
  uint8_t b[4] = {0x78, 0x56, 0x34, 0x12};
  ASSERT_EQ(fram::rd_u32(b, fram::Endian::Little), 0x12345678u);
}

TEST(rd_u32_be_basic) {
  uint8_t b[4] = {0x12, 0x34, 0x56, 0x78};
  ASSERT_EQ(fram::rd_u32(b, fram::Endian::Big), 0x12345678u);
}

TEST(parse_header_le_len_pts_flags_agl) {
  uint8_t h[16]{};
  put_u32_le(h + 0, 100);                         // len
  put_u64_le(h + 4, 0x0000000100000002ULL);       // pts: hi=1, lo=2
  put_u32_le(h + 12, (uint32_t)fram::kKeyframe);  // flags

  fram::Header ph = fram::parse_header(h, fram::Endian::Little);
  ASSERT_EQ(ph.len, 100u);
  ASSERT_EQ(ph.pts_hi, 1u);
  ASSERT_EQ(ph.pts_lo, 2u);
  ASSERT_EQ(ph.flags, (uint32_t)fram::kKeyframe);
}

TEST(parse_header_be_len_pts_flags_agl) {
  uint8_t h[16]{};
  put_u32_be(h + 0, 777);                       // len
  put_u64_be(h + 4, 0x0000000A0000000BULL);     // pts: hi=10 lo=11
  put_u32_be(h + 12, (uint32_t)fram::kConfig);  // flags

  fram::Header ph = fram::parse_header(h, fram::Endian::Big);
  ASSERT_EQ(ph.len, 777u);
  ASSERT_EQ(ph.pts_hi, 10u);
  ASSERT_EQ(ph.pts_lo, 11u);
  ASSERT_EQ(ph.flags, (uint32_t)fram::kConfig);
}

TEST(detect_endian_prefers_le_when_both_plausible) {
  uint8_t h[16]{};
  put_u32_le(h + 0, 0);
  put_u64_le(h + 4, 0);
  put_u32_le(h + 12, 0);
  auto e = fram::detect_endian(h, 1024);
  ASSERT_EQ((int)e, (int)fram::Endian::Little);
}

TEST(detect_endian_selects_be_when_le_implausible) {
  uint8_t h[16]{};
  // bytes: 00 00 00 10 => LE=len huge, BE=len=16
  h[0] = 0x00;
  h[1] = 0x00;
  h[2] = 0x00;
  h[3] = 0x10;
  auto e = fram::detect_endian(h, 1024);
  ASSERT_EQ((int)e, (int)fram::Endian::Big);
}

TEST(flag_name_single_none) {
  ASSERT_STR_EQ(fram::flag_name_single(0), "NONE");
}

TEST(flag_name_single_config) {
  ASSERT_STR_EQ(fram::flag_name_single((uint32_t)fram::kConfig), "CSD");
}

TEST(flag_name_single_keyframe) {
  ASSERT_STR_EQ(fram::flag_name_single((uint32_t)fram::kKeyframe), "IDR");
}

TEST(flag_name_single_eos) {
  ASSERT_STR_EQ(fram::flag_name_single((uint32_t)fram::kEOS), "EOS");
}

TEST(flag_name_single_unknown) {
  auto s = fram::flag_name_single(0x40u);
  ASSERT_TRUE(s.find("UNKNOWN(") != std::string::npos);
}

TEST(decode_flags_none) {
  auto v = fram::decode_flag_names(0);
  ASSERT_EQ(v.size(), (size_t)1);
  ASSERT_STR_EQ(v[0], "NONE");
}

TEST(decode_flags_known_combo) {
  uint32_t f = (uint32_t)fram::kConfig | (uint32_t)fram::kKeyframe;
  auto v = fram::decode_flag_names(f);
  ASSERT_EQ(v.size(), (size_t)2);
  ASSERT_STR_EQ(v[0], "CSD");
  ASSERT_STR_EQ(v[1], "IDR");
}

TEST(decode_flags_known_plus_unknown_grouped) {
  uint32_t f = (uint32_t)fram::kEOS | 0x40u;
  auto v = fram::decode_flag_names(f);
  ASSERT_EQ(v.size(), (size_t)2);
  ASSERT_STR_EQ(v[0], "EOS");
  ASSERT_TRUE(v[1].find("UNKNOWN(") != std::string::npos);
}

TEST(flags_to_string_none) { ASSERT_STR_EQ(fram::flags_to_string(0), "NONE"); }

TEST(flags_to_string_combo_order) {
  uint32_t f = (uint32_t)fram::kConfig | (uint32_t)fram::kKeyframe |
               (uint32_t)fram::kEOS;
  ASSERT_STR_EQ(fram::flags_to_string(f), "CSD|IDR|EOS");
}