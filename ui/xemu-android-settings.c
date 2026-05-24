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

    /* Force the Vulkan renderer; the GL backend is excluded on Android. */
    g_config.display.renderer = CONFIG_DISPLAY_RENDERER_VULKAN;
    g_config.display.quality.surface_scale = 1;
    g_config.display.vulkan.enable_validation = false;
    g_config.display.vulkan.assert_on_validation_msg = false;
    g_config.display.vulkan.validation_layers = NULL;
    g_config.display.vulkan.debug_shaders = false;

    g_config.input.allow_vibration = true;
    g_config.input.gamepad_mappings = NULL;
    g_config.input.gamepad_mappings_count = 0;
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
    return "";
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
