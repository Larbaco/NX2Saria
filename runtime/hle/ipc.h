// Contains code Copyright (c) 2026 MacNativeGames contributors (MIT; see THIRD_PARTY_NOTICES.md).
#pragma once
#include <stdint.h>

#include "host.h"

#define IPC_MAX_BUF 8
#define IPC_MSG_BYTES 0x100
#define IPC_R_BAD_HANDLE 0xE401u
#define IPC_R_UNKNOWN_COMMAND 0xF601u

typedef struct { uint64_t addr, size; unsigned index; } IpcBuffer;

typedef struct {
    unsigned type, cmd;
    int has_pid;
    uint64_t pid;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer x[IPC_MAX_BUF], a[IPC_MAX_BUF], b[IPC_MAX_BUF], w[IPC_MAX_BUF], c[IPC_MAX_BUF];
    unsigned nx, na, nb, nw, nc;
    const uint8_t *data;
    uint32_t data_size;
} IpcRequest;

typedef struct {
    uint32_t result;
    uint8_t data[0x100];
    uint32_t data_size;
    uint32_t copy[8], move[8];
    unsigned ncopy, nmove;
    IpcBuffer xout[IPC_MAX_BUF];
    unsigned nxout;
    int domain;
    unsigned domain_objects;
} IpcResponse;

typedef struct IpcSession IpcSession;
typedef void (*IpcHandler)(IpcSession *s, const IpcRequest *rq, IpcResponse *rs);

struct IpcSession {
    const char *name;
    IpcHandler handler;
    void *state;
    void (*close)(void *state);
    int32_t refs;
    int is_domain;
    IpcSession *root;
    IpcSession *objects[32];
    int nobjects;
    uint32_t event;
    IpcSession *held_root;
    int32_t subs;
    int dom_closed;
};

unsigned ipc_domain_add(IpcSession *domain, IpcSession *object);

uint32_t ipc_session_new(const char *name, IpcHandler handler, void *state, void (*close)(void *state));
void ipc_out(IpcResponse *rs, const void *p, uint32_t n);
const void *ipc_arg(const IpcRequest *rq, uint32_t off, uint32_t n);
void *ipc_guest(uint64_t addr, uint64_t size);
__attribute__((noreturn)) void ipc_unimplemented(const IpcSession *s, const IpcRequest *rq);
#define IPC_OUT(rs, v) ipc_out((rs), &(v), sizeof(v))
#define IPC_ARG(rq, T, off) (*(const T *)ipc_arg((rq), (off), sizeof(T)))

typedef struct { uint64_t addr, size; uint32_t perm; } IpcTransferMemory;

long ipc_sessions_live(void);
uint64_t ipc_sessions_released(void);

void ipc_backtrace(void);
void ipc_applet_event_init(void);
void ipc_applet_push_message(uint32_t msg);
