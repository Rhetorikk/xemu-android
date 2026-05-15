/*
 * libsamplerate stub for the Android foundation pass.
 *
 * The MCPX APU voice processor consumes libsamplerate for audio resampling.
 * Audio output is silent on Android (noaudio backend) for the foundation
 * pass, but the linker still needs the SRC_STATE / src_callback_*
 * symbols. These stubs return "no input available" so the resampler is
 * effectively bypassed.
 *
 * Replace with a real port when audio is wired up.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include <stdlib.h>
#include <string.h>
#include "samplerate.h"

typedef struct stub_state {
    int            channels;
    int            converter_type;
    src_callback_t cb;
    void          *user_data;
} stub_state;

SRC_STATE *src_new(int converter_type, int channels, int *error)
{
    stub_state *s = calloc(1, sizeof(*s));
    if (!s) { if (error) *error = 1; return NULL; }
    s->channels = channels;
    s->converter_type = converter_type;
    if (error) *error = 0;
    return (SRC_STATE *)s;
}

SRC_STATE *src_callback_new(src_callback_t func, int converter_type,
                            int channels, int *error, void *cb_data)
{
    stub_state *s = (stub_state *)src_new(converter_type, channels, error);
    if (!s) return NULL;
    s->cb = func;
    s->user_data = cb_data;
    return (SRC_STATE *)s;
}

SRC_STATE *src_delete(SRC_STATE *state)
{
    free(state);
    return NULL;
}

int src_process(SRC_STATE *state, SRC_DATA *data)
{
    (void)state;
    if (!data) return 0;
    data->input_frames_used = data->input_frames;
    data->output_frames_gen = 0;
    return 0;
}

long src_callback_read(SRC_STATE *state, double src_ratio, long frames,
                       float *data)
{
    (void)state; (void)src_ratio; (void)frames;
    if (data && frames > 0) {
        /* Output silence rather than uninitialized memory */
        memset(data, 0, frames * sizeof(float) * 2);
    }
    return 0;
}

int src_simple(SRC_DATA *data, int converter_type, int channels)
{
    (void)converter_type; (void)channels;
    if (!data) return 0;
    data->input_frames_used = data->input_frames;
    data->output_frames_gen = 0;
    return 0;
}

int src_reset(SRC_STATE *state) { (void)state; return 0; }
int src_set_ratio(SRC_STATE *state, double new_ratio) { (void)state; (void)new_ratio; return 0; }
int src_is_valid_ratio(double ratio) { (void)ratio; return 1; }
int src_error(SRC_STATE *state) { (void)state; return 0; }
const char *src_strerror(int error) { (void)error; return "libsamplerate-stub"; }
const char *src_get_name(int converter_type) { (void)converter_type; return "stub"; }
const char *src_get_description(int converter_type) { (void)converter_type; return "Android foundation stub"; }
const char *src_get_version(void) { return "stub-0"; }
void src_float_to_short_array(const float *in, short *out, int len)
{
    if (out) memset(out, 0, len * sizeof(short));
    (void)in;
}
void src_short_to_float_array(const short *in, float *out, int len)
{
    if (out) memset(out, 0, len * sizeof(float));
    (void)in;
}
void src_float_to_int_array(const float *in, int *out, int len)
{
    if (out) memset(out, 0, len * sizeof(int));
    (void)in;
}
void src_int_to_float_array(const int *in, float *out, int len)
{
    if (out) memset(out, 0, len * sizeof(float));
    (void)in;
}
