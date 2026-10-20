#include <windows.h>
#include <bcrypt.h>
#include "base/random/random.h"
#include <stdexcept>
namespace base {
u64 SourceTrueRandomSeed() {
  u64 seed = 0;
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&seed), sizeof(seed),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("BCryptGenRandom failed");
  return seed;
}
}
