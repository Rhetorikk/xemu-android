/*
 * SDL3 filesystem stubs for the Android foundation build.
 *
 * xemu-settings.cc and xemu-data.c use SDL_GetBasePath / SDL_GetPrefPath
 * / SDL_free to find the executable directory and a user-writable prefs
 * directory. Android has no SDL3 library in the foundation build, but
 * the JNI lifecycle already hands xemu the app's files-dir via JSON
 * config (g_cfg.data_dir). Provide the minimum function set that lets
 * those .cc / .c files link.
 *
 *  - SDL_GetBasePath():   returns the app's nativeLibraryDir-equivalent.
 *                         On Android we don't actually need a base path
 *                         (no portable-mode marker), so return NULL.
 *  - SDL_GetPrefPath(org,app): returns the app's writable filesDir.
 *                         g_cfg.data_dir is set by xemu_android_start.
 *  - SDL_free(p):         plain free().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>

/* xemu-android.c declares this and parses it from the launcher JSON. */
extern const char *xemu_android_data_dir(void);

const char *SDL_GetBasePath(void)
{
    /* No portable-mode marker: signal "use the pref path". */
    return NULL;
}

char *SDL_GetPrefPath(const char *org, const char *app)
{
    (void)org;
    (void)app;
    const char *base = xemu_android_data_dir();
    if (!base) {
        return NULL;
    }
    /* Ensure trailing slash; xemu code concatenates "<pref>filename.toml". */
    size_t len = strlen(base);
    int needs_slash = (len > 0 && base[len - 1] != '/');
    char *out = malloc(len + (needs_slash ? 1 : 0) + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, base, len);
    if (needs_slash) {
        out[len] = '/';
        out[len + 1] = '\0';
    } else {
        out[len] = '\0';
    }
    return out;
}

void SDL_free(void *ptr)
{
    free(ptr);
}
