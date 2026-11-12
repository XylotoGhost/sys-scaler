#include <string.h>
#include "vimag.h"

typedef struct { char data[0x40]; } ViName;

static Result openDisplay(ViMag *v, const char *name) {
    ViName n;
    memset(&n, 0, sizeof(n));
    strncpy(n.data, name, sizeof(n.data) - 1);
    Result rc = serviceDispatchInOut(&v->app, 1010, n, v->display_id);
    if (R_SUCCEEDED(rc)) strncpy(v->display_name, name, sizeof(v->display_name) - 1);
    return rc;
}

Result vimagOpen(ViMag *v, Result *first_rc) {
    memset(v, 0, sizeof(*v));
    if (first_rc) *first_rc = 0;

    Result rc = smGetService(&v->root, "vi:m");
    if (R_FAILED(rc)) return rc;

    // GetDisplayService for vi:m is cmd 2, input 1 = privileged.
    const u32 inval = 1;
    rc = serviceDispatchIn(&v->root, 2, inval, .out_num_objects = 1, .out_objects = &v->app);
    if (R_FAILED(rc)) { serviceClose(&v->root); return rc; }

    rc = serviceDispatch(&v->app, 101, .out_num_objects = 1, .out_objects = &v->sys);
    if (R_FAILED(rc)) { serviceClose(&v->app); serviceClose(&v->root); return rc; }

    rc = openDisplay(v, "Default");
    if (R_FAILED(rc)) {
        if (first_rc) *first_rc = rc;
        rc = openDisplay(v, "External");
    }
    if (R_FAILED(rc)) {
        serviceClose(&v->sys); serviceClose(&v->app); serviceClose(&v->root);
        return rc;
    }
    v->open = true;
    return 0;
}

void vimagClose(ViMag *v) {
    if (!v->open) return;
    serviceDispatchIn(&v->app, 1020, v->display_id);
    serviceClose(&v->sys);
    serviceClose(&v->app);
    serviceClose(&v->root);
    v->open = false;
}

Result vimagGetLogicalResolution(ViMag *v, s32 *w, s32 *h) {
    struct { s32 w, h; } out = {0};
    Result rc = serviceDispatchInOut(&v->sys, 1203, v->display_id, out);
    if (R_SUCCEEDED(rc)) { *w = out.w; *h = out.h; }
    return rc;
}

Result vimagSet(ViMag *v, s32 x, s32 y, s32 w, s32 h) {
    const struct { s32 x, y, w, h; u64 display_id; } in = { x, y, w, h, v->display_id };
    return serviceDispatchIn(&v->sys, 1204, in);
}
