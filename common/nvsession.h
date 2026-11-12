// Minimal standalone nvdrv client.
//
// libnx's nv.c keeps one global session whose service name (nvdrv / nvdrv:a /
// nvdrv:s / nvdrv:t) is fixed at first init. The console framebuffer already
// uses that global session, and only nvdrv:s / nvdrv:t carry permission bit 8
// ("Display", required for /dev/nvdisp-*). So we open our own, independent
// sessions here. IPC layout mirrors libnx nv.c (cmd 0 Open, 1 Ioctl, 2 Close,
// 3 Initialize, 8 SetAruid).
#pragma once
#include <switch.h>

typedef struct {
    Service        srv;
    TransferMemory tmem;
    const char    *name;
    bool           active;
    u32            last_nv_error; // raw NvError of the last call (0 = ok)
} NvSession;

// Opens the named nv service and runs Initialize (+ SetAruid when we have one).
// *aruid_rc receives the SetAruid result (0 if not attempted). May be NULL.
Result nvsOpen(NvSession *s, const char *service_name, u32 tmem_size, Result *aruid_rc);
void   nvsClose(NvSession *s);

Result nvsDevOpen(NvSession *s, const char *path, u32 *fd);
Result nvsDevClose(NvSession *s, u32 fd);
// Buffer size and direction are derived from the request code, like libnx nvIoctl.
Result nvsIoctl(NvSession *s, u32 fd, u32 request, void *argp);
