/*
 * Minimal xemu settings shim for the Android foundation build.
 *
 * The desktop build uses ui/xemu-settings.cc for TOML-backed persistent
 * configuration plus the gamepad-binding store. That file pulls in SDL3
 * filesystem APIs and the controllers module, neither of which is wired
 * for the Android port yet.
 *
 * Provides:
 *   - the global `struct config g_config` with safe defaults
 *   - no-op implementations of the xemu_settings_* API surface that
 *     anything in libxemu.so might reference
 *
 * The settings struct itself is defined by subprojects/genconfig from
 * config_spec.yml; we just have to instantiate it. nv2a, mcpx and the
 * xbox machine read fields directly off g_config (e.g.
 * g_config.display.renderer), so providing the struct unblocks linkage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "xemu-settings.h"

struct config g_config;
static int g_config_initialized = 0;
static char *g_settings_path;
static char g_settings_error[256] = "";

static void init_defaults_once(void)
{
    if (g_config_initialized) {
        return;
    }
    g_config_initialized = 1;

    /*
     * On desktop, genconfig's config_tree seeds g_config from config_spec.yml
     * defaults inside xemu_settings_load(). That C++ path is excluded on
     * Android, so g_config is just zero-initialized - which would leave the
     * renderer at NULL, surface_scale at 0, and every string field NULL. We
     * replicate the boot- and render-critical defaults here by hand.
     */

    /* Force the Vulkan renderer; the GL backend is excluded on Android. */
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    g_config.display.quality.surface_scale = 1;
    g_config.display.vulkan.validation_layers = false;
    g_config.display.vulkan.assert_on_validation_msg = false;
    g_config.display.vulkan.debug_shaders = false;

    g_config.input.allow_vibration = true;
    g_config.input.gamepad_mappings = NULL;
    g_config.input.gamepad_mappings_count = 0;

    /*
     * sys defaults. mem_limit 0 == "64" (MB), the retail Xbox default, so the
     * zero-init is already correct. avpack defaults to HDTV on desktop;
     * zero-init would select SCART, so set it explicitly.
     */
    g_config.sys.avpack = CONFIG_SYS_AVPACK_HDTV;

    /*
     * Every sys.files.* path must be a valid (possibly empty) string: vl.c's
     * qemu_init() calls strlen()/strdup_double_commas() on them directly while
     * assembling the xbox machine. xemu_settings_set_string() free()s the old
     * pointer (NULL is fine) and strdup()s the new value. The launcher
     * overwrites these with the user-picked paths before qemu_init() runs.
     */
    xemu_settings_set_string(&g_config.sys.files.bootrom_path, "");
    xemu_settings_set_string(&g_config.sys.files.flashrom_path, "");
    xemu_settings_set_string(&g_config.sys.files.eeprom_path, "");
    xemu_settings_set_string(&g_config.sys.files.hdd_path, "");
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");
    xemu_settings_set_string(&g_config.display.vulkan.preferred_physical_device,
                             "");

    /*
     * perf defaults are both true on desktop. hard_fpu selects the faster
     * host-FPU TCG helpers (target/i386/tcg/translate.c reads it); cache_shaders
     * is GL-only and unused on the Vulkan path, but we keep it consistent.
     */
    g_config.perf.hard_fpu = true;
    g_config.perf.cache_shaders = true;

    /*
     * Must be false: vl.c forces autostart off when show_welcome is set (the
     * desktop "first boot, let the user configure paths" guard). On Android the
     * launcher only spawns emulation once BIOS + flash are chosen, so we want
     * the machine to auto-start.
     */
    g_config.general.show_welcome = false;
}

void xemu_settings_set_path(const char *path)
{
    free(g_settings_path);
    g_settings_path = path ? strdup(path) : NULL;
}

const char *xemu_settings_get_base_path(void)
{
    extern const char *xemu_android_data_dir(void);
    const char *d = xemu_android_data_dir();
    return d ? d : "/data/local/tmp/xemu";
}

const char *xemu_settings_get_path(void)
{
    return g_settings_path ? g_settings_path : "";
}

const char *xemu_settings_get_default_eeprom_path(void)
{
    /*
     * vl.c's get_eeprom_path() falls back here when the launcher didn't supply
     * an EEPROM. It then xbox_eeprom_generate()s a fresh 256-byte EEPROM at
     * this path, so it must point at a writable location (the app's data dir).
     * Returning "" would make generation fail and force autostart off, so the
     * machine would never boot. Mirror the desktop "<base>/eeprom.bin".
     */
    static char *eeprom_path = NULL;
    if (eeprom_path != NULL) {
        return eeprom_path;
    }

    const char *base = xemu_settings_get_base_path();
    size_t blen = strlen(base);
    const char *sep = (blen > 0 && base[blen - 1] == '/') ? "" : "/";
    eeprom_path = g_strdup_printf("%s%seeprom.bin", base, sep);
    return eeprom_path;
}

const char *xemu_settings_get_error_message(void)
{
    return g_settings_error;
}

bool xemu_settings_load(void)
{
    init_defaults_once();
    return true;
}

void xemu_settings_save(void)
{
    /* No-op: persistence comes when full settings TOML lands. */
}

void add_net_nat_forward_ports(int host, int guest,
                               CONFIG_NET_NAT_FORWARD_PORTS_PROTOCOL protocol)
{
    (void)host;
    (void)guest;
    (void)protocol;
}

void remove_net_nat_forward_ports(unsigned int index)
{
    (void)index;
}

bool xemu_settings_load_gamepad_mapping(const char *guid,
                                        GamepadMappings **mapping)
{
    (void)guid;
    if (mapping) {
        *mapping = NULL;
    }
    return false;
}

void xemu_settings_reset_controller_mapping(const char *guid)
{
    (void)guid;
}

void xemu_settings_reset_keyboard_mapping(void)
{
}
