#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <stdint.h>

typedef enum {
    VIS_OFF = 0,
    VIS_VU_METERS,
    VIS_SPECTRUM,
    VIS_COUNT
} vis_mode_t;

#define VIS_MAX_CHANNELS 40
#define VIS_SPECTRUM_BANDS 24
#define VIS_FFT_SIZE 512
#define VIS_BAR_WIDTH 40

typedef struct {
    double peak;
    double rms;
    double decay_peak;
    char label[32];
    int active;
} vu_channel_t;

typedef struct {
    double magnitude;
    double decay;
    double freq_lo;
    double freq_hi;
} spectrum_band_t;

typedef struct {
    vis_mode_t mode;
    int num_channels;
    vu_channel_t channels[VIS_MAX_CHANNELS];
    spectrum_band_t bands[VIS_SPECTRUM_BANDS];
    int num_bands;
    double fft_input[VIS_FFT_SIZE];
    double fft_real[VIS_FFT_SIZE];
    double fft_imag[VIS_FFT_SIZE];
    int fft_pos;
    int sample_rate;
    int console_width;
    int console_height;
    int frame_counter;
    int update_interval;
} visualizer_t;

void vis_init(visualizer_t* vis, int sample_rate, int num_channels);
void vis_set_channel_label(visualizer_t* vis, int ch, const char* label);
void vis_feed_samples(visualizer_t* vis, int16_t* stereo_buf, int num_frames);
void vis_update_channel(visualizer_t* vis, int ch, double level, int active);
void vis_cycle_mode(visualizer_t* vis);
void vis_render(visualizer_t* vis);
const char* vis_mode_name(visualizer_t* vis);

void move_cursor(int x, int y);
void hide_cursor(void);
void show_cursor(void);
void clear_screen(void);
void get_console_size(int* w, int* h);

#endif
