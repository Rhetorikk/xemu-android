/*
 * xemu Android entry point and lifecycle bridge
 *
 * Copyright (c) 2026 xemu contributors
 *
 * Provides the JNI-callable entry points that drive the emulator lifecycle.
 * xemu_android_start() seeds g_config, spawns the Vulkan presenter thread
 * (ui/xemu-android-display.c) and, when BIOS + flash are configured, the QEMU
 * emulation thread (qemu_init -> qemu_main_loop). The presenter blits the
 * nv2a Vulkan display image onto the Android swapchain.
 *
 * This file is the Android sibling of ui/xemu.c's main(). It deliberately
 * does NOT reuse the GL display path; it relies on ui/xemu-android-display.c
 * for presentation. The machine itself is built by vl.c from g_config (see
 * apply_launcher_config), exactly as on the desktop.
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
#include "ui/console.h"
#include "ui/surface.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"
#include "xemu-settings.h"

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
 * Push the launcher-picked paths into g_config so that vl.c's qemu_init()
 * assembles the xbox machine correctly.
 *
 * This is the crux of the integration. xemu does NOT build its machine from
 * the caller's argv: qemu_init() (system/vl.c) constructs its own argv from
 * g_config.sys.files.* - the -machine xbox,bootrom=..., -bios, the
 * smbus-storage EEPROM device, and the hdd/dvd -drive entries are all derived
 * there, then the caller's argv[1..] is *appended*. Passing our own
 * -machine/-bios/-drive would therefore duplicate those options and QEMU would
 * reject the command line. So we populate g_config and pass a bare argv.
 *
 * Only non-empty launcher fields overwrite the "" defaults seeded by
 * xemu_settings_load(); leaving eeprom empty lets get_eeprom_path() generate
 * one, and leaving dvd empty leaves the drive present with no media.
 */
static void apply_launcher_config(void)
{
    if (g_cfg.bios && *g_cfg.bios) {
        xemu_settings_set_string(&g_config.sys.files.bootrom_path, g_cfg.bios);
    }
    if (g_cfg.flash && *g_cfg.flash) {
        xemu_settings_set_string(&g_config.sys.files.flashrom_path, g_cfg.flash);
    }
    if (g_cfg.eeprom && *g_cfg.eeprom) {
        xemu_settings_set_string(&g_config.sys.files.eeprom_path, g_cfg.eeprom);
    }
    if (g_cfg.hdd && *g_cfg.hdd) {
        xemu_settings_set_string(&g_config.sys.files.hdd_path, g_cfg.hdd);
    }
    if (g_cfg.dvd && *g_cfg.dvd) {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, g_cfg.dvd);
    }

    LOGI("apply_launcher_config: bootrom=%s flash=%s eeprom=%s hdd=%s dvd=%s",
         g_config.sys.files.bootrom_path, g_config.sys.files.flashrom_path,
         g_config.sys.files.eeprom_path, g_config.sys.files.hdd_path,
         g_config.sys.files.dvd_path);
}

/*
 * DCL callbacks. dpy_gfx_switch fires when QEMU's display surface is
 * created or resized; dpy_gfx_update fires on each dirty-region update.
 * For the foundation we log basic stats so we can verify the nv2a is
 * actually producing frames. A future pass copies the surface pixels
 * into the Vulkan swapchain.
 */
static atomic_int g_frame_count;
static atomic_int g_surface_w, g_surface_h;

static void android_dpy_gfx_switch(DisplayChangeListener *dcl,
                                    DisplaySurface *new_surface)
{
    (void)dcl;
    if (!new_surface) {
        LOGI("dpy_gfx_switch: surface cleared");
        atomic_store(&g_surface_w, 0);
        atomic_store(&g_surface_h, 0);
        return;
    }
    int w = surface_width(new_surface);
    int h = surface_height(new_surface);
    atomic_store(&g_surface_w, w);
    atomic_store(&g_surface_h, h);
    LOGI("dpy_gfx_switch: new surface %dx%d, stride=%d, format=0x%x",
         w, h, surface_stride(new_surface),
         (unsigned)surface_format(new_surface));
}

static void android_dpy_gfx_update(DisplayChangeListener *dcl,
                                    int x, int y, int w, int h)
{
    (void)dcl; (void)x; (void)y; (void)w; (void)h;
    int n = atomic_fetch_add(&g_frame_count, 1) + 1;
    if ((n & 0x3f) == 0) {
        LOGI("dpy_gfx_update: %d frame updates received (this one was %dx%d "
             "at %d,%d)", n, w, h, x, y);
    }
}

static void *qemu_main_thread(void *opaque)
{
    (void)opaque;

    /*
     * Bare argv, mirroring the desktop ui/xemu.c which calls
     * qemu_init(gArgc, gArgv) with essentially just the program name. vl.c
     * derives the entire xbox machine from g_config (populated in
     * apply_launcher_config) and appends our argv[1..] afterwards, so we pass
     * nothing here. argv[0] is still used for error_init()/exec-dir setup.
     */
    char *argv[] = { (char *)"xemu", NULL };
    int argc = 1;

    LOGI("qemu_main_thread: calling qemu_init (machine built from g_config)");
    qemu_init(argc, argv);
    LOGI("qemu_main_thread: qemu_init returned");

    /* Register a minimal DisplayChangeListener so we can observe whether
     * the machine actually produced a display surface. The desktop ui/
     * code registers per-console; we do it on console 0 only, which is
     * the nv2a output on the xbox machine. */
    QemuConsole *con = qemu_console_lookup_by_index(0);
    if (con) {
        static const DisplayChangeListenerOps android_dcl_ops = {
            .dpy_name        = "xemu-android",
            .dpy_gfx_switch  = android_dpy_gfx_switch,
            .dpy_gfx_update  = android_dpy_gfx_update,
        };
        static DisplayChangeListener android_dcl;
        android_dcl.ops = &android_dcl_ops;
        android_dcl.con = con;
        register_displaychangelistener(&android_dcl);
        LOGI("qemu_main_thread: registered xemu-android DCL on console 0");
    } else {
        LOGW("qemu_main_thread: no console index 0 - nv2a may not have "
             "initialized; display will stay blank");
    }

    LOGI("qemu_main_thread: entering qemu_main_loop");
    g_qemu_exit_status = qemu_main_loop();
    LOGI("qemu_main_thread: qemu_main_loop returned %d", g_qemu_exit_status);

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

    /*
     * Seed g_config with the Android defaults (renderer=Vulkan, non-NULL file
     * paths, show_welcome=false, ...). On desktop this is where the TOML config
     * loads; our stub just installs safe defaults. Must run before qemu_init
     * touches g_config on the emulation thread.
     */
    xemu_settings_load();

    if (config_json) {
        LOGI("xemu_android_start: config_json (%zu bytes)", strlen(config_json));
        parse_config(config_json);
    }

    /* Translate the launcher's picked paths into g_config.sys.files.*. */
    apply_launcher_config();

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
