/*
 * Minimal libsamplerate header for the Android foundation stub.
 *
 * Mirrors the upstream libsamplerate.h ABI for the subset of symbols xemu
 * consumes. Used only when host_os == 'android' and the system libsamplerate
 * is unavailable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef XEMU_ANDROID_SAMPLERATE_H
#define XEMU_ANDROID_SAMPLERATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SRC_STATE_tag SRC_STATE;

typedef struct {
    const float *data_in;
    float       *data_out;
    long         input_frames;
    long         output_frames;
    long         input_frames_used;
    long         output_frames_gen;
    int          end_of_input;
    double       src_ratio;
} SRC_DATA;

typedef long (*src_callback_t)(void *cb_data, float **data);

enum {
    SRC_SINC_BEST_QUALITY    = 0,
    SRC_SINC_MEDIUM_QUALITY  = 1,
    SRC_SINC_FASTEST         = 2,
    SRC_ZERO_ORDER_HOLD      = 3,
    SRC_LINEAR               = 4,
};

SRC_STATE *src_new(int converter_type, int channels, int *error);
SRC_STATE *src_callback_new(src_callback_t func, int converter_type,
                            int channels, int *error, void *cb_data);
SRC_STATE *src_delete(SRC_STATE *state);
int        src_process(SRC_STATE *state, SRC_DATA *data);
long       src_callback_read(SRC_STATE *state, double src_ratio, long frames,
                             float *data);
int        src_simple(SRC_DATA *data, int converter_type, int channels);
int        src_reset(SRC_STATE *state);
int        src_set_ratio(SRC_STATE *state, double new_ratio);
int        src_is_valid_ratio(double ratio);
int        src_error(SRC_STATE *state);
const char *src_strerror(int error);
const char *src_get_name(int converter_type);
const char *src_get_description(int converter_type);
const char *src_get_version(void);
void       src_float_to_short_array(const float *in, short *out, int len);
void       src_short_to_float_array(const short *in, float *out, int len);
void       src_float_to_int_array(const float *in, int *out, int len);
void       src_int_to_float_array(const int *in, float *out, int len);

#ifdef __cplusplus
}
#endif

#endif
