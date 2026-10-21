/*
 * Fixed-width integer types for the AVDTP core. The kernel-mode CRT in the
 * WDK has no <stdint.h>, so the driver build (_KERNEL_MODE) defines them here.
 */
#ifndef AVDTP_TYPES_H
#define AVDTP_TYPES_H

#include <stddef.h>

#ifdef _KERNEL_MODE
typedef unsigned __int8 uint8_t;
typedef unsigned __int16 uint16_t;
typedef unsigned __int32 uint32_t;
typedef unsigned __int64 uint64_t;
typedef __int8 int8_t;
typedef __int16 int16_t;
typedef __int32 int32_t;
typedef __int64 int64_t;
#else
#include <stdint.h>
#endif

#endif /* AVDTP_TYPES_H */
