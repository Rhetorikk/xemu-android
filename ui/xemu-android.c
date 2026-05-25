/*
 * xemu Android entry point and lifecycle bridge
 *
 * Copyright (c) 2026 xemu contributors
 *
 * Foundation pass: provides JNI-callable entry points that initialize SDL3 +
 * Vulkan, run a render loop that clears the SurfaceView, and shut down
 * cleanly. The actual `qemu_init` / `qemu_main_loop` invocation is wired but
 * gated behind a config flag because the nv2a Vulkan renderer is not yet
 * rewired to present to the Android swapchain (next pass).
 *
 * This file is the Android sibling of ui/xemu.c's main(). It deliberately
 * does NOT reuse the GL display path; it relies on ui/xemu-android-display.c
 * for presentation.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction.
 */

#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/runstate.h"
#include "system/system.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"

#include <android/log.h>
#include <android/native_window.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "xemu"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* From system/vl.c — same entry points the desktop main() uses. */
extern void qemu_init(int argc, char **argv);
extern int  qemu_main_loop(void);
extern void qemu_cleanup(int);

extern void xemu_android_display_set_window(ANativeWindow *win);
extern int  xemu_android_display_run(void);
extern void xemu_android_display_request_stop(void);

/*
 * Parsed launcher config. JSON keys come from
 * android/app/src/main/java/app/xemu/LauncherActivity.kt.
 *
 * Pointers are NULL if the field was absent or empty. All strings are owned
 * by g_cfg and freed in xemu_android_shutdown.
 */
typedef struct XemuAndroidConfig {
    char *bios;
    char *flash;
    char *eeprom;
    char *hdd;
    char *dvd;
    char *data_dir;
} XemuAndroidConfig;

static XemuAndroidConfig g_cfg;
static atomic_int g_running = 0;
static pthread_t g_emu_thread;
static pthread_t g_qemu_thread;
static atomic_int g_qemu_started = 0;
static int g_qemu_exit_status = 0;

/* Exposed for ui/xemu-android-sdl-stubs.c -> SDL_GetPrefPath. */
const char *xemu_android_data_dir(void)
{
    return g_cfg.data_dir;
}

/*
 * Build the QEMU command line from the launcher-supplied config. Returns a
 * heap-allocated argv[] (NULL-terminated); *argc is set. Caller frees with
 * free_qemu_argv(). Empty fields are skipped.
 *
 * The resulting layout for a fully-configured boot is:
 *
 *   xemu \
 *     -machine xbox,bootrom=<mcpx>,short_animation=on \
 *     -bios <flash.bin> \
 *     -cpu pentium3 -m 64 -smp 1 -accel tcg \
 *     -drive index=0,media=disk,file=<hdd>,if=ide \
 *     -drive index=1,media=cdrom,file=<dvd>,if=ide \
 *     -nodefaults -display none
 *
 * Note: -display none means QEMU won't open a window. Frame data still
 * reaches our Vulkan presenter via the nv2a Vulkan renderer's framebuffer
 * image (next pass: actually wire that through).
 */
static char **build_qemu_argv(int *argc_out)
{
    /* Generous upper bound on number of slots. */
    const int max_args = 32;
    char **argv = calloc(max_args, sizeof(char *));
    int n = 0;
    argv[n++] = strdup("xemu");

    if (g_cfg.bios && *g_cfg.bios) {
        argv[n++] = strdup("-machine");
        char *m = NULL;
        if (asprintf(&m, "xbox,bootrom=%s,short_animation=on", g_cfg.bios) > 0) {
            argv[n++] = m;
        } else {
            argv[n++] = strdup("xbox");
        }
    } else {
        argv[n++] = strdup("-machine");
        argv[n++] = strdup("xbox");
    }

    if (g_cfg.flash && *g_cfg.flash) {
        argv[n++] = strdup("-bios");
        argv[n++] = strdup(g_cfg.flash);
    }

    argv[n++] = strdup("-cpu");      argv[n++] = strdup("pentium3");
    argv[n++] = strdup("-m");        argv[n++] = strdup("64");
    argv[n++] = strdup("-smp");      argv[n++] = strdup("1");
    argv[n++] = strdup("-accel");    argv[n++] = strdup("tcg");

    if (g_cfg.hdd && *g_cfg.hdd) {
        argv[n++] = strdup("-drive");
        char *d = NULL;
        if (asprintf(&d, "index=0,media=disk,file=%s,if=ide", g_cfg.hdd) > 0) {
            argv[n++] = d;
        }
    }
    if (g_cfg.dvd && *g_cfg.dvd) {
        argv[n++] = strdup("-drive");
        char *d = NULL;
        if (asprintf(&d, "index=1,media=cdrom,file=%s,if=ide", g_cfg.dvd) > 0) {
            argv[n++] = d;
        }
    }

    argv[n++] = strdup("-nodefaults");
    argv[n++] = strdup("-display");  argv[n++] = strdup("none");

    *argc_out = n;
    return argv;
}

static void free_qemu_argv(char **argv, int argc)
{
    if (!argv) return;
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
}

static void *qemu_main_thread(void *opaque)
{
    (void)opaque;
    int argc = 0;
    char **argv = build_qemu_argv(&argc);

    LOGI("qemu_main_thread: argv (%d args):", argc);
    for (int i = 0; i < argc; i++) {
        LOGI("  argv[%d] = %s", i, argv[i]);
    }

    LOGI("qemu_main_thread: calling qemu_init");
    qemu_init(argc, argv);
    LOGI("qemu_main_thread: qemu_init returned, entering main loop");
    g_qemu_exit_status = qemu_main_loop();
    LOGI("qemu_main_thread: qemu_main_loop returned %d", g_qemu_exit_status);

    free_qemu_argv(argv, argc);
    return NULL;
}

/*
 * Tiny pull-style JSON string extractor. Looks for "key":"value" pairs in
 * the input and copies the value if found. Returns 1 if a non-empty value
 * was stored, 0 otherwise. Good enough for our flat 6-key config object;
 * intentionally avoids pulling in nlohmann_json (which lives in the
 * desktop UI subset that's excluded on Android).
 */
static int json_get_string(const char *json, const char *key, char **out)
{
    *out = NULL;
    if (!json || !key) return 0;
    size_t klen = strlen(key);
    const char *p = json;
    while ((p = strstr(p, key)) != NULL) {
        if (p == json || *(p - 1) != '"') { p++; continue; }
        if (p[klen] != '"') { p++; continue; }
        const char *q = p + klen + 1;
        while (*q == ' ' || *q == '\t' || *q == ':') q++;
        if (*q != '"') { p++; continue; }
        q++;
        const char *end = q;
        while (*end && *end != '"') {
            if (*end == '\\' && end[1]) end++;
            end++;
        }
        if (*end != '"') return 0;
        size_t len = end - q;
        if (len == 0) return 0;
        char *copy = malloc(len + 1);
        if (!copy) return 0;
        memcpy(copy, q, len);
        copy[len] = 0;
        *out = copy;
        return 1;
    }
    return 0;
}

static void parse_config(const char *json)
{
    if (!json) return;
    json_get_string(json, "bios",     &g_cfg.bios);
    json_get_string(json, "flash",    &g_cfg.flash);
    json_get_string(json, "eeprom",   &g_cfg.eeprom);
    json_get_string(json, "hdd",      &g_cfg.hdd);
    json_get_string(json, "dvd",      &g_cfg.dvd);
    json_get_string(json, "data_dir", &g_cfg.data_dir);

    LOGI("config.bios     = %s", g_cfg.bios     ? g_cfg.bios     : "(unset)");
    LOGI("config.flash    = %s", g_cfg.flash    ? g_cfg.flash    : "(unset)");
    LOGI("config.eeprom   = %s", g_cfg.eeprom   ? g_cfg.eeprom   : "(unset)");
    LOGI("config.hdd      = %s", g_cfg.hdd      ? g_cfg.hdd      : "(unset)");
    LOGI("config.dvd      = %s", g_cfg.dvd      ? g_cfg.dvd      : "(unset)");
    LOGI("config.data_dir = %s", g_cfg.data_dir ? g_cfg.data_dir : "(unset)");
}

static void free_config(void)
{
    free(g_cfg.bios);     g_cfg.bios     = NULL;
    free(g_cfg.flash);    g_cfg.flash    = NULL;
    free(g_cfg.eeprom);   g_cfg.eeprom   = NULL;
    free(g_cfg.hdd);      g_cfg.hdd      = NULL;
    free(g_cfg.dvd);      g_cfg.dvd      = NULL;
    free(g_cfg.data_dir); g_cfg.data_dir = NULL;
}

/*
 * Foundation-pass main loop driver. Bypasses qemu_init() for now and just
 * pumps the Vulkan display. The next pass replaces this body with the same
 * pattern as ui/xemu.c's main(): spawn qemu_main thread, wait on
 * display_init_sem, run poll_events + render frame.
 */
static void *xemu_android_thread(void *opaque)
{
    (void)opaque;
    LOGI("xemu_android_thread: start");

    int rc = xemu_android_display_run();
    LOGI("display_run returned %d", rc);

    atomic_store(&g_running, 0);
    return NULL;
}

int xemu_android_start(const char *config_json, ANativeWindow *win)
{
    if (atomic_exchange(&g_running, 1)) {
        LOGW("xemu_android_start: already running");
        return -1;
    }

    LOGI("xemu_android_start: version=%s", xemu_version);
    LOGI("xemu_android_start: os=%s", xemu_get_os_info());

    if (config_json) {
        LOGI("xemu_android_start: config_json (%zu bytes)", strlen(config_json));
        parse_config(config_json);
    }

    xemu_android_display_set_window(win);

    int err = pthread_create(&g_emu_thread, NULL, xemu_android_thread, NULL);
    if (err) {
        LOGE("pthread_create (display) failed: %d", err);
        atomic_store(&g_running, 0);
        return -2;
    }
    pthread_setname_np(g_emu_thread, "xemu-main");

    /*
     * Spawn the QEMU emulation thread only when BIOS + flash are both set.
     * Without them, qemu_init() will abort with "kernel ROM image not
     * found"; better to leave the display in clear-only mode than crash
     * the Activity.
     */
    if (g_cfg.bios && *g_cfg.bios && g_cfg.flash && *g_cfg.flash) {
        err = pthread_create(&g_qemu_thread, NULL, qemu_main_thread, NULL);
        if (err) {
            LOGE("pthread_create (qemu) failed: %d", err);
        } else {
            pthread_setname_np(g_qemu_thread, "qemu_main");
            atomic_store(&g_qemu_started, 1);
        }
    } else {
        LOGW("BIOS or flash missing - skipping qemu_init; display will only "
             "clear the surface. Pick MCPX + flash in the launcher to boot.");
    }
    return 0;
}

void xemu_android_set_surface(ANativeWindow *win)
{
    LOGI("xemu_android_set_surface: %p", (void *)win);
    xemu_android_display_set_window(win);
}

void xemu_android_pause(void)
{
    LOGI("xemu_android_pause");
}

void xemu_android_resume(void)
{
    LOGI("xemu_android_resume");
}

void xemu_android_shutdown(void)
{
    LOGI("xemu_android_shutdown");
    if (!atomic_load(&g_running)) {
        return;
    }
    xemu_android_display_request_stop();
    pthread_join(g_emu_thread, NULL);
    if (atomic_load(&g_qemu_started)) {
        /* Best-effort shutdown signal; qemu's main loop should observe
         * the powerdown request and exit. If it doesn't, we'd block
         * forever - so detach instead of join. */
        qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
        pthread_detach(g_qemu_thread);
    }
    free_config();
}

/*
 * Stub controller wiring. The JNI exports call these; xemu-input integration
 * comes after the on-screen overlay is built.
 */
void xemu_android_send_button(int controller, int button, int down)
{
    (void)controller; (void)button; (void)down;
}

void xemu_android_send_axis(int controller, int axis, float value)
{
    (void)controller; (void)axis; (void)value;
}
