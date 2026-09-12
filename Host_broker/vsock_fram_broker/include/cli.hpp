
#pragma once
#include <string>

#include "broker.hpp"
struct CliResult {
  bool ok = false;
  BrokerConfig cfg{};
  std::string error;
  bool help = false;
};
void print_help(const char* prog);
// Non-terminating parse: never calls exit(). Great for unit tests.
CliResult parse_args(int argc, char** argv);
// Production helper: exits on error/help, returns config otherwise.
BrokerConfig parse_args_or_die(int argc, char** argv);