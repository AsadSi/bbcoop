/* The general registers of a signal context as one array, g[REG_RIP] and so on, on Linux and
 * macOS: the fault and trap handlers read and change them the same way on both.
 * macOS (x86-64): the thread state (__ss) holds rax..r15, rip and rflags as consecutive 64-bit
 * fields, so it is indexed with REG_* values in that order. */
#ifndef BBPORT_UCONTEXT_H
#define BBPORT_UCONTEXT_H
#ifdef __APPLE__
#include <signal.h>
#include <sys/ucontext.h>
typedef long long greg_t;
enum {
    REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RDI, REG_RSI, REG_RBP, REG_RSP,
    REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
    REG_RIP, REG_EFL
};
#define BB_GREGS(uc) ((greg_t *)&(uc)->uc_mcontext->__ss)
#else
#include <ucontext.h>
#define BB_GREGS(uc) ((uc)->uc_mcontext.gregs)
#endif
#endif
