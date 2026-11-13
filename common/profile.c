#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include "profile.h"
#include "settings.h"

u32 profileKey(const NvdcModeDB2 *db) {
    u32 n = db->num_modes > 201 ? 201 : db->num_modes;
    u32 crc = crc32Calculate(&n, sizeof(n));
    return crc32CalculateWithSeed(crc, db->modes, n * sizeof(NvdcMode2));
}

static void profilePath(u32 key, char *buf, size_t n) {
    snprintf(buf, n, SS_PROFILE_DIR "/%08X.ini", key);
}

const char *profileMethodName(ProfileMethod m) {
    switch (m) {
        case PROFILE_CENTERED: return "Centered";
        case PROFILE_SHARP:    return "Sharp";
        default:               return "Off";
    }
}

bool profileLoad(u32 key, Profile *out) {
    char path[96], line[256];
    profilePath(key, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool have_mode = false, have_zoom = false;
    char method[16] = "sharp";
    memset(out, 0, sizeof(*out));
    while (fgets(line, sizeof(line), f)) {
        u32 *v = (u32 *)&out->mode;
        s32 *z = out->zoom;
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "method=%15s", method) == 1) continue;
        if (sscanf(line, "aspect=%u:%u", &out->ar_w, &out->ar_h) == 2) continue;
        if (strncmp(line, "name=", 5) == 0) { snprintf(out->name, sizeof(out->name), "%.15s", line + 5); continue; }
        if (!out->name[0]) {   // prototype header: # AspectFix profile for "NAME" (...)
            const char *q = strstr(line, "for \"");
            if (line[0] == '#' && q) {
                q += 5;
                size_t n = strcspn(q, "\"");
                if (n >= sizeof(out->name)) n = sizeof(out->name) - 1;
                memcpy(out->name, q, n);
                out->name[n] = 0;
                continue;
            }
        }
        if (sscanf(line, "zoom=%d,%d,%d,%d", &z[0], &z[1], &z[2], &z[3]) == 4) { have_zoom = true; continue; }
        if (sscanf(line, "mode2=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
                   &v[10], &v[11], &v[12], &v[13], &v[14]) == 15)
            have_mode = true;
    }
    fclose(f);
    if (strcmp(method, "off") == 0 || strcmp(method, "none") == 0) {
        out->method = PROFILE_OFF;
        return true;
    }
    if (strcmp(method, "centered") == 0 || strcmp(method, "zoom") == 0) {
        out->method = PROFILE_CENTERED;
        return have_zoom && out->zoom[2] > 0 && out->zoom[3] > 0;
    }
    out->method = PROFILE_SHARP;
    return have_mode && out->mode.hActive && out->mode.vActive && out->mode.pclkKHz;
}

bool profileSave(u32 key, const Profile *p) {
    ssMkdirs();
    char path[96];
    profilePath(key, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const u32 *v = (const u32 *)&p->mode;
    fprintf(f, "# sys-scaler profile (mode DB key %08X). Delete this file to forget the monitor.\n", key);
    fprintf(f, "name=%s\n", p->name[0] ? p->name : "Unknown monitor");
    fprintf(f, "method=%s\n", p->method == PROFILE_CENTERED ? "centered" : p->method == PROFILE_SHARP ? "sharp" : "off");
    if (p->ar_w && p->ar_h) fprintf(f, "aspect=%u:%u\n", p->ar_w, p->ar_h);
    if (p->method == PROFILE_CENTERED)
        fprintf(f, "zoom=%d,%d,%d,%d\n", p->zoom[0], p->zoom[1], p->zoom[2], p->zoom[3]);
    else if (p->method == PROFILE_SHARP)
        fprintf(f, "mode2=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n", v[0], v[1], v[2], v[3], v[4],
                v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14]);
    fclose(f);
    return true;
}

bool profileDelete(u32 key) {
    char path[96];
    profilePath(key, path, sizeof(path));
    return unlink(path) == 0;
}

int profileList(u32 *keys, int max) {
    DIR *d = opendir(SS_PROFILE_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    unsigned key;
    while ((e = readdir(d)) && n < max) {
        char ext[8] = "";
        if (sscanf(e->d_name, "%8X.%7s", &key, ext) == 2 && strcmp(ext, "ini") == 0) keys[n++] = key;
    }
    closedir(d);
    return n;
}

int profileDeleteAll(void) {
    u32 keys[PROFILE_MAX];
    int n = profileList(keys, PROFILE_MAX), removed = 0;
    for (int i = 0; i < n; i++)
        if (profileDelete(keys[i])) removed++;
    return removed;
}
