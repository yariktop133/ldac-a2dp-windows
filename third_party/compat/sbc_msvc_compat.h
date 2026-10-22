/* Force-included into the AOSP SBC encoder under MSVC (see CMakeLists.txt).
 * sbc_packing.c multiplies into an int64_t with __builtin_mul_overflow, which
 * cannot overflow there: a plain 64-bit multiply is equivalent. */
#pragma once
#include <stdint.h>
#define __builtin_mul_overflow(a, b, res) (*(res) = (int64_t)(a) * (int64_t)(b), 0)
