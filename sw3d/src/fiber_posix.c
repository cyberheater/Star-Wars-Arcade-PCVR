/* fiber_posix.c - see fiber_posix.h.  The stacks are mapped, not committed:
 * the reserve the main program asks for (256 MB) costs only what it touches.
 */
#define _XOPEN_SOURCE 700
#define _DARWIN_C_SOURCE
#include <stdlib.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "fiber_posix.h"

typedef struct {
    ucontext_t ctx;
    void      *stack;
    size_t     stack_size;
    void     (*fn)(LPVOID);
    LPVOID     param;
} fiber;

static fiber *current;
static fiber *starting;

static void fiber_entry(void)
{
    fiber *f = starting;
    f->fn(f->param);
    abort();                    /* a fiber's function does not return */
}

LPVOID ConvertThreadToFiber(LPVOID param)
{
    fiber *f = (fiber *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->param = param;
    current = f;
    return f;
}

LPVOID CreateFiberEx(size_t commit, size_t reserve, unsigned flags, void (*fn)(LPVOID), LPVOID param)
{
    (void)commit; (void)flags;
    fiber *f = (fiber *)calloc(1, sizeof *f);
    if (!f) return NULL;
    f->stack_size = reserve;
    f->stack = mmap(NULL, reserve, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (f->stack == MAP_FAILED) { free(f); return NULL; }
    f->fn = fn;
    f->param = param;
    getcontext(&f->ctx);
    f->ctx.uc_stack.ss_sp = f->stack;
    f->ctx.uc_stack.ss_size = reserve;
    f->ctx.uc_link = NULL;
    makecontext(&f->ctx, fiber_entry, 0);
    return f;
}

void SwitchToFiber(LPVOID to)
{
    fiber *from = current, *f = (fiber *)to;
    if (f == from) return;
    current = f;
    starting = f;               /* read once, by a fiber on its first switch */
    swapcontext(&from->ctx, &f->ctx);
}

void DeleteFiber(LPVOID p)
{
    fiber *f = (fiber *)p;
    if (!f || f == current) return;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}
