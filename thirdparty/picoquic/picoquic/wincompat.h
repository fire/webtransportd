/*
* Author: Christian Huitema
* Copyright (c) 2017, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef WINCOMPAT_H
#define WINCOMPAT_H

/* MinGW's _mingw.h defines __CRT_INLINE which ws2tcpip.h uses for IN6_*
 * helpers.  On some cross-compile toolchains __CRT_INLINE resolves to
 * "extern __inline__" (no __gnu_inline__), which in C99+ emits a real
 * external definition in every TU → multiple-definition link errors.
 * Include _mingw.h first (burns its include guard so Winsock2.h won't
 * re-trigger it), then force __CRT_INLINE to use GNU inline semantics
 * which never emit standalone copies. */
#ifdef __MINGW32__
#  include <_mingw.h>
#  undef __CRT_INLINE
#  define __CRT_INLINE extern inline __attribute__((__gnu_inline__))
#endif

#include <stdint.h>
#define ssize_t int
#include <Winsock2.h>

#ifndef gettimeofday
#define gettimeofday wintimeofday

#ifndef __attribute__
#define __attribute__(X)
#endif

#pragma warning(disable : 4214)

#ifdef __cplusplus
extern "C" {
#endif

    /* MinGW's time.h already defines struct timezone */
#ifndef __MINGW32__
    struct timezone {
        int tz_minuteswest;     /* minutes west of Greenwich */
        int tz_dsttime;         /* type of DST correction */
    };
#endif

    int wintimeofday(struct timeval* tv, struct timezone* tz);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

#endif /* WINCOMPAT_H */
