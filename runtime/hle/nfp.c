#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "amiibo.h"
#include "host.h"
#include "ipc.h"
#include "kernel.h"
#include "nfp.h"

static const uint8_t g_device_handle[8] = { 1, 0, 0, 0, 0, 0, 0, 0 };

typedef struct {
    uint32_t activate_event;
    uint32_t deactivate_event;
    uint32_t avail_event;
} NfpUserState;

static void nfp_user_close(void *p) {
    NfpUserState *st = p;
    if (st) {
        if (st->activate_event) kern_handle_close(st->activate_event);
        if (st->deactivate_event) kern_handle_close(st->deactivate_event);
        if (st->avail_event) kern_handle_close(st->avail_event);
        free(st);
    }
    rt_log("nfp: IUser session closed");
}

static void nfp_user_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    NfpUserState *st = (NfpUserState *)s->state;

    switch (rq->cmd) {
    case 0: {
        rt_log("nfp: Initialize");
        rs->result = 0;
        return;
    }
    case 1: {
        rt_log("nfp: Finalize");
        amiibo_stop_detection();
        amiibo_unmount();
        rs->result = 0;
        return;
    }
    case 2: {
        int32_t count = amiibo_is_device_present() ? 1 : 0;
        IPC_OUT(rs, count);
        if (count > 0 && rq->nb > 0 && rq->b[0].size >= sizeof(g_device_handle)) {
            void *dst = ipc_guest(rq->b[0].addr, sizeof(g_device_handle));
            memcpy(dst, g_device_handle, sizeof(g_device_handle));
        }
        rs->result = 0;
        return;
    }
    case 3: {
        amiibo_start_detection();
        if (amiibo_has_tag() && st && st->activate_event) {
            kern_event_signal(st->activate_event);
        }
        rs->result = 0;
        return;
    }
    case 4: {
        amiibo_stop_detection();
        rs->result = 0;
        return;
    }
    case 5: {
        if (!amiibo_has_tag()) {
            rs->result = 0x8073u;
            return;
        }
        amiibo_mount();
        rs->result = 0;
        return;
    }
    case 6: {
        amiibo_unmount();
        if (st && st->deactivate_event) {
            kern_event_signal(st->deactivate_event);
        }
        rs->result = 0;
        return;
    }
    case 7: {
        rs->result = amiibo_open_app_area(IPC_ARG(rq, uint32_t, 8));
        return;
    }
    case 8: {
        void *dst = NULL;
        uint64_t cap = 0, n = 0;
        if (rq->nb > 0 && rq->b[0].size > 0) {
            dst = ipc_guest(rq->b[0].addr, rq->b[0].size);
            cap = rq->b[0].size;
        }
        rs->result = amiibo_get_app_area(dst, cap, &n);
        if (!rs->result) {
            uint32_t area_size = (uint32_t)n;
            IPC_OUT(rs, area_size);
        }
        return;
    }
    case 9: {
        const void *src = NULL;
        uint64_t size = 0;
        if (rq->na > 0 && rq->a[0].size > 0) {
            src = ipc_guest(rq->a[0].addr, rq->a[0].size);
            size = rq->a[0].size;
        }
        rs->result = amiibo_set_app_area(src, size);
        return;
    }
    case 10: {
        rs->result = amiibo_flush_figure();
        return;
    }
    case 11: {
        rs->result = amiibo_is_mounted() ? 0 : AMIIBO_R_DEVICE_NOT_FOUND;
        return;
    }
    case 12:
    case 24: {
        const void *src = NULL;
        uint64_t size = 0;
        if (rq->na > 0 && rq->a[0].size > 0) {
            src = ipc_guest(rq->a[0].addr, rq->a[0].size);
            size = rq->a[0].size;
        }
        rs->result = amiibo_create_app_area(IPC_ARG(rq, uint32_t, 8), src, size, rq->cmd == 24);
        return;
    }
    case 13: {
        if (!amiibo_has_tag() && !amiibo_is_mounted()) {
            rs->result = 0x8073u;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x58) {
            void *dst = ipc_guest(rq->b[0].addr, 0x58);
            amiibo_get_tag_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 14: {
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x100) {
            void *dst = ipc_guest(rq->b[0].addr, 0x100);
            amiibo_get_register_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 15: {
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x40) {
            void *dst = ipc_guest(rq->b[0].addr, 0x40);
            amiibo_get_common_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 16: {
        if (!amiibo_is_mounted()) {
            rs->result = 0x927Fu;
            return;
        }
        if (rq->nb > 0 && rq->b[0].size >= 0x40) {
            void *dst = ipc_guest(rq->b[0].addr, 0x40);
            amiibo_get_model_info(dst);
        }
        rs->result = 0;
        return;
    }
    case 17: {
        if (st && st->activate_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->activate_event);
        }
        rs->result = 0;
        return;
    }
    case 18: {
        if (st && st->deactivate_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->deactivate_event);
        }
        rs->result = 0;
        return;
    }
    case 19: {
        uint32_t state = 1;
        IPC_OUT(rs, state);
        rs->result = 0;
        return;
    }
    case 20: {
        uint32_t dev_state = (uint32_t)amiibo_get_device_state();
        IPC_OUT(rs, dev_state);
        rs->result = 0;
        return;
    }
    case 21: {
        uint32_t npad_id = 0;
        IPC_OUT(rs, npad_id);
        rs->result = 0;
        return;
    }
    case 22: {
        uint32_t size = 0xD8;
        IPC_OUT(rs, size);
        rs->result = 0;
        return;
    }
    case 23: {
        if (st && st->avail_event) {
            rs->copy[rs->ncopy++] = kern_event_readable(st->avail_event);
        }
        rs->result = 0;
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}

void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    switch (rq->cmd) {
    case 0: {
        NfpUserState *st = calloc(1, sizeof(*st));
        if (!st) rt_fatal("nfp: failed to allocate user state");
        st->activate_event = kern_event_new();
        st->deactivate_event = kern_event_new();
        st->avail_event = kern_event_new();
        kern_event_signal(st->avail_event);

        uint32_t user_session = ipc_session_new("nfp:IUser", nfp_user_handler, st, nfp_user_close);
        rs->move[rs->nmove++] = user_session;
        rs->result = 0;
        rt_log("nfp: created IUser session handle %#x", user_session);
        return;
    }
    default:
        ipc_unimplemented(s, rq);
    }
}
