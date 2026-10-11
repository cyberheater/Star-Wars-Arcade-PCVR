/* fiber_posix.h - the four Win32 fiber calls port.cpp uses, on ucontext, for
 * hosts that are not Windows.  Each of the two programs is a loop that never
 * returns, so each needs a stack of its own; see port.cpp.
 */
#ifndef SW_FIBER_POSIX_H
#define SW_FIBER_POSIX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *LPVOID;
#define VOID     void
#define CALLBACK

LPVOID ConvertThreadToFiber(LPVOID param);
LPVOID CreateFiberEx(size_t commit, size_t reserve, unsigned flags, void (*fn)(LPVOID), LPVOID param);
void   SwitchToFiber(LPVOID fiber);
void   DeleteFiber(LPVOID fiber);

#ifdef __cplusplus
}
#endif
#endif
