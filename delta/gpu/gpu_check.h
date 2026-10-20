/*
 * PS4Delta : PS4/PS5 emulation and research project
 */
#pragma once

#include <cstdlib>
#include "base/logging.h"
#if defined(DELTA_UWP)
#include "embedding/host.h"
#define GPU_CHECK_FAILURE() embedding::Fail("GPU invariant failed; see prosperity.log")
#else
#define GPU_CHECK_FAILURE() std::abort()
#endif

#define GPU_BUGCHECK(expression, ...)                                          \
  do {                                                                         \
    if (!(expression)) {                                                       \
      BASE_LOGI("gpu", "BUGCHECK {}:{}: {}", __FILE__, __LINE__, #expression); \
      __VA_OPT__(BASE_LOGI("gpu", "  " __VA_ARGS__);)                          \
      GPU_CHECK_FAILURE();                                                     \
    }                                                                          \
  } while (0)
