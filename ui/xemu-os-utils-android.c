/*
 * OS-specific Helpers (Android)
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-os-utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/system_properties.h>
#include <glib.h>

const char *xemu_get_os_info(void)
{
    static char buf[256];
    static int initialized = 0;

    if (!initialized) {
        char release[PROP_VALUE_MAX] = {0};
        char model[PROP_VALUE_MAX] = {0};
        char manufacturer[PROP_VALUE_MAX] = {0};

        __system_property_get("ro.build.version.release", release);
        __system_property_get("ro.product.model", model);
        __system_property_get("ro.product.manufacturer", manufacturer);

        g_snprintf(buf, sizeof buf, "Android %s (%s %s)",
                   release[0] ? release : "?",
                   manufacturer[0] ? manufacturer : "",
                   model[0] ? model : "device");
        initialized = 1;
    }
    return buf;
}
