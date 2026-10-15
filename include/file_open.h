#pragma once
#include <stdio.h>

// Newlib in the pinned ESP32 toolchain supports x -> O_EXCL.
// fopen opens the stream directly, without fdopen's F_GETFL dependency.
// Keep exclusive creation: an existing log must never be truncated.
inline FILE *openExclusiveLog(const char *path) {
    return fopen(path, "wbx");
}
