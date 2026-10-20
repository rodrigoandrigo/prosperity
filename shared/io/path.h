#pragma once

/*
 * UTL : The universal utility library
 *
 * Copyright 2019-2020 Force67.
 * For information regarding licensing see LICENSE
 * in the root of the source tree.
 */

#include "base/strings/xstring.h"

namespace io {
#if defined(DELTA_UWP)
// Set by the DLL host before starting guest workers. Never use the package's
// read-only executable directory for saves, firmware or caches.
void SetDataDirectory(const char* utf8_directory);
#endif
base::StringW MakeAbsPath(const base::StringW& relative);
base::String MakeAbsPath(const base::String& relative);
}  // namespace io
