
#include <test_harness.hpp>

#include "cli.hpp"
static CliResult parse_vec(const std::vector<const char*>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (auto* s : args) argv.push_back(const_cast<char*>(s));
  return parse_args((int)argv.size(), argv.data());
}
TEST(cli_defaults_ok) {
  auto r = parse_vec({"prog"});
  ASSERT_TRUE(r.ok);
  ASSERT_FALSE(r.help);
  ASSERT_EQ(r.cfg.host_listen_port, 5000u);
  ASSERT_EQ(r.cfg.android_cid, 3u);
  ASSERT_EQ(r.cfg.android_port, 22345u);
  ASSERT_EQ(r.cfg.android_connect_timeout_ms, 1500);
  ASSERT_EQ(r.cfg.retry_sleep_ms, 500);
  ASSERT_FALSE(r.cfg.verbose_headers);
}
TEST(cli_help_flag) {
  auto r = parse_vec({"prog", "--help"});
  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.help);
}
TEST(cli_set_android_cid) {
  auto r = parse_vec({"prog", "--android-cid", "3"});
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.cfg.android_cid, 3u);
}
TEST(cli_set_ports) {
  auto r = parse_vec(
      {"prog", "--host-listen-port", "6000", "--android-port", "3333"});
  ASSERT_TRUE(r.ok);
  ASSERT_EQ(r.cfg.host_listen_port, 6000u);
  ASSERT_EQ(r.cfg.android_port, 3333u);
}
TEST(cli_verbose_headers) {
  auto r = parse_vec({"prog", "--verbose-headers"});
  ASSERT_TRUE(r.ok);
  ASSERT_TRUE(r.cfg.verbose_headers);
}
TEST(cli_missing_value_errors) {
  auto r = parse_vec({"prog", "--android-cid"});
  ASSERT_FALSE(r.ok);
  ASSERT_TRUE(r.error.find("Missing value") != std::string::npos);
}
TEST(cli_invalid_number_errors) {
  auto r = parse_vec({"prog", "--android-port", "abc"});
  ASSERT_FALSE(r.ok);
  ASSERT_TRUE(r.error.find("Invalid") != std::string::npos);
}
TEST(cli_unknown_arg_errors) {
  auto r = parse_vec({"prog", "--nope"});
  ASSERT_FALSE(r.ok);
  ASSERT_TRUE(r.error.find("Unknown arg") != std::string::npos);
}