/* This file contains an implementation of the Eisel-Lemire algorithm, as
 * described in https://nigeltao.github.io/blog/2020/eisel-lemire.html. Much of
 * the implementation is based on the one available in Wuffs
 * (https://github.com/google/wuffs/blob/c104ae296c3557f946e4bd5ee8b85511f12c141c/internal/cgen/base/floatconv-submodule-code.c#L989),
 * the license of which is copied below:
 *
 * """
 * Copyright 2020 The Wuffs Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * """
 *
 * */

#ifndef MS_ATOF_H
#define MS_ATOF_H

#include <stdint.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "atof_consts.h"

typedef struct ms_uint128 {
    uint64_t lo;
    uint64_t hi;
} ms_uint128;

static inline ms_uint128
ms_mulu64(uint64_t x, uint64_t y) {
#if defined(__SIZEOF_INT128__)
    ms_uint128 out;
    __uint128_t z = ((__uint128_t)x) * ((__uint128_t)y);
    out.lo = (uint64_t)z;
    out.hi = (uint64_t)(z >> 64);
    return out;
#else
    ms_uint128 out;
    uint64_t x0 = x & 0xFFFFFFFF;
    uint64_t x1 = x >> 32;
    uint64_t y0 = y & 0xFFFFFFFF;
    uint64_t y1 = y >> 32;
    uint64_t w0 = x0 * y0;
    uint64_t t = (x1 * y0) + (w0 >> 32);
    uint64_t w1 = t & 0xFFFFFFFF;
    uint64_t w2 = t >> 32;
    w1 += x0 * y1;
    out.lo = x * y;
    out.hi = (x1 * y1) + w2 + (w1 >> 32);
    return out;
#endif
}

static inline uint32_t
ms_clzll(uint64_t x) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64) || defined(_M_IA64))
  uint32_t index = 0;
  _BitScanReverse64(&index, x);
  return (int)(63 - index);
#elif defined(__GNUC__)
  return (uint32_t)__builtin_clzll(x);
#else
    uint32_t out;
    if ((x >> 32) == 0) {
        out |= 32;
        x <<= 32;
    }
    if ((x >> 48) == 0) {
        out |= 16;
        x <<= 16;
    }
    if ((x >> 56) == 0) {
        out |= 8;
        x <<= 8;
    }
    if ((x >> 60) == 0) {
        out |= 4;
        x <<= 4;
    }
    if ((x >> 62) == 0) {
        out |= 2;
        x <<= 2;
    }
    if ((x >> 63) == 0) {
        out |= 1;
        x <<= 1;
    }
    return out;
#endif
}

static inline bool
reconstruct_double(uint64_t man, int32_t exp, bool is_negative, double *out) {
    /* If both `man` and `10 ** exp` can be exactly represented as a double, we
     * can take a fast path */
    if ((-22 <= exp) && (exp <= 22) && ((man >> 53) == 0)) {
        double d = (double)man;
        if (exp >= 0) {
            d *= ms_atof_f64_powers_of_10[exp];
        } else {
            d /= ms_atof_f64_powers_of_10[-exp];
        }
        *out = is_negative ? -d : d;
        return true;
    }

    /* Special case 0 handling. This is only hit if the mantissa is 0 and the
     * exponent is out of bounds above (i.e. rarely) */
    if (man == 0) {
        *out = is_negative ? -0.0 : 0.0;
        return true;
    }

    /* The short comment headers below correspond to section titles in Nigel
     * Tao's blogpost. See
     * https://nigeltao.github.io/blog/2020/eisel-lemire.html for a more
     * in-depth description of the algorithm */

    /* Normalization */
    const uint64_t* po10 = ms_atof_powers_of_10[exp + 307];
    uint32_t clz = ms_clzll(man);
    man <<= clz;
    uint64_t ret_exp2 = ((uint64_t)(((217706 * exp) >> 16) + 1087)) - ((uint64_t)clz);

    /* Multiplication */
    ms_uint128 x = ms_mulu64(man, po10[1]);
    uint64_t x_hi = x.hi;
    uint64_t x_lo = x.lo;

    /* Apply a wider Approximation if needed */
	if (((x_hi & 0x1FF) == 0x1FF) && ((x_lo + man) < man)) {
		ms_uint128 y = ms_mulu64(man, po10[0]);
		uint64_t y_hi = y.hi;
		uint64_t y_lo = y.lo;

		uint64_t merged_hi = x_hi;
		uint64_t merged_lo = x_lo + y_hi;
		if (merged_lo < x_lo) {
            merged_hi++;
		}

        /* If the result is still ambiguous at this approximation, abort */
		if (((merged_hi & 0x1FF) == 0x1FF) && ((merged_lo + 1) == 0) && (y_lo + man < man)) {
            return false;
		}

		x_hi = merged_hi;
		x_lo = merged_lo;
	}

    /* Shift to 54 bits */
	uint64_t msb = x_hi >> 63;
	uint64_t ret_mantissa = x_hi >> (msb + 9);
	ret_exp2 -= 1 ^ msb;

    /* Check for a half-way ambiguity, and abort if present */
	if ((x_lo == 0) && ((x_hi & 0x1FF) == 0) && ((ret_mantissa & 3) == 1)) {
		return false;
	}

    /* From 54 to 53 bits */
	ret_mantissa += ret_mantissa & 1;
	ret_mantissa >>= 1;
	if ((ret_mantissa >> 53) > 0) {
		ret_mantissa >>= 1;
		ret_exp2++;
	}

    /* Construct final output */
	ret_mantissa &= 0x000FFFFFFFFFFFFF;
	uint64_t ret = ret_mantissa | (ret_exp2 << 52) | (((uint64_t)is_negative) << 63);
    memcpy(out, &ret, sizeof(double));
    return true;
}

#endif
