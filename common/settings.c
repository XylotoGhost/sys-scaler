#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "settings.h"

void settingsDefaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->enabled = true;
    s->refresh_s = REFRESH_DEFAULT;
}

bool settingsLoad(Settings *s) {
    settingsDefaults(s);
    FILE *f = fopen(SS_CONFIG, "r");
    if (!f) return false;
    char line[128];
    int v;
    unsigned u;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "enabled=%d", &v) == 1) s->enabled = v != 0;
        else if (sscanf(line, "refresh=%d", &v) == 1) s->refresh_s = (v >= 0 && v <= 3600) ? v : REFRESH_DEFAULT;
        else if (sscanf(line, "expert=%d", &v) == 1) s->expert = v != 0;
        else if (sscanf(line, "rev=%u", &u) == 1) s->rev = u;
    }
    fclose(f);
    return true;
}

bool settingsSave(Settings *s) {
    ssMkdirs();
    s->rev++;
    FILE *f = fopen(SS_CONFIG, "w");
    if (!f) return false;
    fprintf(f, "# sys-scaler settings (the app writes this file)\n");
    fprintf(f, "# enabled: 1 = fix monitors that have a saved profile, 0 = do nothing\n");
    fprintf(f, "enabled=%d\n", s->enabled ? 1 : 0);
    fprintf(f, "# refresh: re-apply the centered zoom every N seconds (0 = only after docking).\n");
    fprintf(f, "# The system resets it after boot and in the power menu, so keep this at 1.\n");
    fprintf(f, "refresh=%d\n", s->refresh_s);
    fprintf(f, "expert=%d\n", s->expert ? 1 : 0);
    fprintf(f, "rev=%u\n", s->rev);
    fclose(f);
    return true;
}

u32 settingsFileCrc(void) {
    FILE *f = fopen(SS_CONFIG, "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return crc32Calculate(buf, n) | 1;   // never 0 for an existing file
}

void ssMkdirs(void) {
    mkdir("sdmc:/config", 0777);
    mkdir(SS_DIR, 0777);
    mkdir(SS_PROFILE_DIR, 0777);
}

static bool copyFile(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    if (!in) return false;
    FILE *out = fopen(to, "wb");
    if (!out) { fclose(in); return false; }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return true;
}

int settingsMigrateLegacy(void) {
    if (fileExists(SS_CONFIG)) return -1;
    ssMkdirs();
    Settings s;
    settingsDefaults(&s);
    int n = 0;
    DIR *d = opendir(SS_LEGACY_DIR "/profiles");
    if (d) {
        struct dirent *e;
        unsigned key;
        char from[320], to[320];
        while ((e = readdir(d))) {
            if (sscanf(e->d_name, "%8X.txt", &key) != 1) continue;
            snprintf(from, sizeof(from), SS_LEGACY_DIR "/profiles/%s", e->d_name);
            snprintf(to, sizeof(to), SS_PROFILE_DIR "/%08X.ini", key);
            if (!fileExists(to) && copyFile(from, to)) n++;
        }
        closedir(d);
    }
    if (fileExists(SS_LEGACY_DIR "/disabled")) s.enabled = false;
    // refresh.txt from the prototype is deliberately not taken over: run 8 showed
    // that 0 breaks the fix after boot and the power menu.
    settingsSave(&s);
    return n;
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
    ssMkdirs();
    FILE *f = fopen(SS_PAUSE_FILE, "w");
    if (!f) return false;
    fprintf(f, "%llu\n", (unsigned long long)pid);
    fclose(f);
    return true;
}

bool pauseReadPid(u64 *pid) {
    FILE *f = fopen(SS_PAUSE_FILE, "r");
    if (!f) return false;
    unsigned long long v = 0;
    bool ok = fscanf(f, "%llu", &v) == 1 && v;
    fclose(f);
    *pid = v;
    return ok;
}

void pauseRemoveIfOurs(void) {
    u64 mine = 0, pid = 0;
    svcGetProcessId(&mine, CUR_PROCESS_HANDLE);
    if (pauseReadPid(&pid) && pid == mine) unlink(SS_PAUSE_FILE);
}
