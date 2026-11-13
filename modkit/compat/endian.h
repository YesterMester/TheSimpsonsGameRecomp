#pragma once
// The shared SHA-256 source includes endian.h outside MSVC. MinGW's x64
// target is little-endian and does not ship the POSIX header.
#if defined(_WIN32)
#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __BYTE_ORDER __LITTLE_ENDIAN
#else
#include_next <endian.h>
#endif
