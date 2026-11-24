#pragma once
#include "host.h"

#define KERN_R_OUT_OF_RESOURCE 0xCE01u
#define KERN_R_INVALID_STATE_MEM 0xD401u
#define KERN_R_BAD_HANDLE 0xE401u
#define KERN_R_TIMEOUT 0xEA01u
#define KERN_R_CANCELLED 0xEC01u
#define KERN_R_INVALID_ENUM 0xF001u
#define KERN_R_BAD_STATE 0xFA01u
#define KERN_H_CURRENT_THREAD 0xFFFF8000u
#define KERN_WAITERS_BIT 0x40000000u

#define KERN_SYM_INITIALIZE_FIBER "_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi"
#define KERN_SYM_FINALIZE_FIBER "_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE"
#define KERN_SYM_SWITCH_TO_FIBER "_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE"
#define KERN_SYM_GET_CURRENT_FIBER "_ZN2nn2os15GetCurrentFiberEv"
void kern_fiber_initialize(Cpu *c);
void kern_fiber_finalize(Cpu *c);
void kern_fiber_switch(Cpu *c);
void kern_fiber_current(Cpu *c);

uint64_t kern_threads_created(void);
void kern_seed_tls(void);

void kern_backtrace(const Cpu *c);

enum { KERN_WAIT_NONE, KERN_WAIT_LOCK, KERN_WAIT_CONDVAR, KERN_WAIT_ADDRESS, KERN_WAIT_SYNC };
#define KERN_THREAD_EXITED 100
int kern_thread_state(uint32_t handle);
void kern_core_stats(int *held_now, int *held_max);
void kern_core_stats_reset(void);
int kern_cores(void);
extern volatile int kern_test_cv_old_order;
extern void (*volatile kern_test_cv_hook)(void);
extern void (*volatile svc_test_exit_hook)(unsigned code);
