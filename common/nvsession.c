#include <string.h>
#include "nvsession.h"

#define NV_IOC_DIR(x)  (((x) >> 30) & 3)
#define NV_IOC_SIZE(x) (((x) >> 16) & 0x3FFF)
#define NV_IOC_WRITE   1
#define NV_IOC_READ    2

static Result nvsResult(NvSession *s, u32 nv_error) {
    s->last_nv_error = nv_error;
    return nv_error ? nvConvertError((int)nv_error) : 0;
}

Result nvsOpen(NvSession *s, const char *service_name, u32 tmem_size, Result *aruid_rc) {
    memset(s, 0, sizeof(*s));
    s->name = service_name;
    if (aruid_rc) *aruid_rc = 0;

    Result rc = smGetService(&s->srv, service_name);
    if (R_FAILED(rc)) return rc;

    rc = tmemCreate(&s->tmem, tmem_size, Perm_None);
    if (R_FAILED(rc)) {
        serviceClose(&s->srv);
        return rc;
    }

    u32 nv_err = 0;
    rc = serviceDispatchInOut(&s->srv, 3, tmem_size, nv_err,
        .in_num_handles = 2,
        .in_handles = { CUR_PROCESS_HANDLE, s->tmem.handle },
    );
    // nvservices keeps its own reference to the memory; libnx closes the handle here too.
    tmemCloseHandle(&s->tmem);
    if (R_SUCCEEDED(rc)) rc = nvsResult(s, nv_err);

    if (R_FAILED(rc)) {
        serviceClose(&s->srv);
        tmemWaitForPermission(&s->tmem, Perm_Rw);
        tmemClose(&s->tmem);
        return rc;
    }

    u64 aruid = appletGetAppletResourceUserId();
    if (aruid) {
        // Non-fatal: sysmodules (nvdrv:s users) normally don't send one.
        Result arc = serviceDispatchIn(&s->srv, 8, aruid, .in_send_pid = true);
        if (aruid_rc) *aruid_rc = arc;
    }

    s->active = true;
    return 0;
}

void nvsClose(NvSession *s) {
    if (!s->active) return;
    serviceClose(&s->srv);
    tmemWaitForPermission(&s->tmem, Perm_Rw);
    tmemClose(&s->tmem);
    s->active = false;
}

Result nvsDevOpen(NvSession *s, const char *path, u32 *fd) {
    struct { u32 fd; u32 error; } out = {0};
    Result rc = serviceDispatchOut(&s->srv, 0, out,
        .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
        .buffers = { { path, strlen(path) } },
    );
    if (R_SUCCEEDED(rc)) rc = nvsResult(s, out.error);
    if (R_SUCCEEDED(rc) && fd) *fd = out.fd;
    return rc;
}

Result nvsDevClose(NvSession *s, u32 fd) {
    u32 err = 0;
    Result rc = serviceDispatchInOut(&s->srv, 2, fd, err);
    if (R_SUCCEEDED(rc)) rc = nvsResult(s, err);
    return rc;
}

Result nvsIoctl(NvSession *s, u32 fd, u32 request, void *argp) {
    size_t size = NV_IOC_SIZE(request);
    u32 dir = NV_IOC_DIR(request);

    void *send = NULL, *recv = NULL;
    size_t send_size = 0, recv_size = 0;
    if (dir & NV_IOC_WRITE) { send = argp; send_size = size; }
    if (dir & NV_IOC_READ)  { recv = argp; recv_size = size; }

    const struct { u32 fd; u32 request; } in = { fd, request };
    u32 err = 0;
    Result rc = serviceDispatchInOut(&s->srv, 1, in, err,
        .buffer_attrs = {
            SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
            SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
        },
        .buffers = {
            { send, send_size },
            { recv, recv_size },
        },
    );
    if (R_SUCCEEDED(rc)) rc = nvsResult(s, err);
    return rc;
}
