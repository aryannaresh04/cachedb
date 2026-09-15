#pragma once

#include <string>

#include "resp.h"
#include "store.h"

namespace cachedb
{

  // Runs one parsed command against the store and appends its reply to `out`.
  //
  // Never fails the connection. An unknown command, bad arity or unsupported
  // option is an ordinary error reply and the client carries on; only the
  // parser can decide framing is lost.
  void execute(const Command &cmd, Store &store, std::string &out);

} // namespace cachedb
