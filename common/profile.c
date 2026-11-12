#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "profile.h"

u32 profileKey(const NvdcModeDB2 *db) {
    u32 n = db->num_modes > 201 ? 201 : db->num_modes;
    u32 crc = crc32Calculate(&n, sizeof(n));
    return crc32CalculateWithSeed(crc, db->modes, n * sizeof(NvdcMode2));
}

static void profilePath(u32 key, char *buf, size_t n) {
    snprintf(buf, n, AF_PROFILE_DIR "/%08X.txt", key);
}

bool fileExists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool touchFile(const char *path) {
    FILE *f = fopen(path, "a");
    if (!f) return false;
    fclose(f);
    return true;
}

bool pauseWrite(void) {
    u64 pid = 0;
    svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    mkdir(AF_DIR, 0777);
    FILE *f = fopen(AF_PAUSE_FILE, "w");
    if (!f) return false;
    fprintf(f, "%llu\n", (unsigned long long)pid);
    fclose(f);
    return true;
}

bool pauseReadPid(u64 *pid) {
    FILE *f = fopen(AF_PAUSE_FILE, "r");
    if (!f) return false;
    unsigned long long v = 0;
    bool ok = fscanf(f, "%llu", &v) == 1 && v;
    fclose(f);
    *pid = v;
    return ok;
}

bool profileLoad(u32 key, Profile *out) {
    char path[96], line[256];
    profilePath(key, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool have_mode = false, have_zoom = false;
    char method[16] = "mode";
    memset(out, 0, sizeof(*out));
    out->base_w = 1920;
    out->base_h = 1080;
    while (fgets(line, sizeof(line), f)) {
        u32 *v = (u32 *)&out->mode;
        s32 *z = out->zoom;
        if (sscanf(line, "method=%15s", method) == 1) continue;
        if (sscanf(line, "base=%ux%u", &out->base_w, &out->base_h) == 2) continue;
        if (sscanf(line, "zoom=%d,%d,%d,%d", &z[0], &z[1], &z[2], &z[3]) == 4) { have_zoom = true; continue; }
        if (sscanf(line, "mode2=%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9],
                   &v[10], &v[11], &v[12], &v[13], &v[14]) == 15)
            have_mode = true;
    }
    fclose(f);
    if (strcmp(method, "zoom") == 0) {
        out->method = PROFILE_ZOOM;
        return have_zoom && out->zoom[2] > 0 && out->zoom[3] > 0;
    }
    out->method = PROFILE_MODE;
    return have_mode && out->mode.hActive && out->mode.vActive && out->mode.pclkKHz;
}

bool profileSave(u32 key, const Profile *p, const char *monitor_name) {
    mkdir(AF_DIR, 0777);
    mkdir(AF_PROFILE_DIR, 0777);
    char path[96];
    profilePath(key, path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const u32 *v = (const u32 *)&p->mode;
    fprintf(f, "# AspectFix profile for \"%s\" (mode DB key %08X)\n", monitor_name ? monitor_name : "?", key);
    fprintf(f, "# delete this file to stop applying it\n");
    fprintf(f, "method=%s\n", p->method == PROFILE_ZOOM ? "zoom" : "mode");
    fprintf(f, "base=%ux%u\n", p->base_w, p->base_h);
    if (p->method == PROFILE_ZOOM)
        fprintf(f, "zoom=%d,%d,%d,%d\n", p->zoom[0], p->zoom[1], p->zoom[2], p->zoom[3]);
    else
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

int profileDeleteAll(void) {
    DIR *d = opendir(AF_PROFILE_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    char path[320];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), AF_PROFILE_DIR "/%s", e->d_name);
        if (unlink(path) == 0) n++;
    }
    closedir(d);
    return n;
}
