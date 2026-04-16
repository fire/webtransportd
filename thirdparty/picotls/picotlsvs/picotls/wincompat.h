#ifndef WINCOMPAT_H
#define WINCOMPAT_H

#include <stdint.h>
#define ssize_t int
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
#include <Winsock2.h>
#include <ws2tcpip.h>
#include <malloc.h>

#ifndef gettimeofday
#define gettimeofday wintimeofday

#ifndef __attribute__
#define __attribute__(X)
#endif

#ifdef __cplusplus
extern "C" {
#endif
/* MinGW's time.h already defines struct timezone */
#ifndef __MINGW32__
struct timezone {
    int tz_minuteswest; /* minutes west of Greenwich */
    int tz_dsttime;     /* type of DST correction */
};
#endif

int wintimeofday(struct timeval *tv, struct timezone *tz);

#ifndef strcasecmp
#define strcasecmp _stricmp
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif

#endif /* WINCOMPAT_H */
