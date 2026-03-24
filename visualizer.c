#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "visualizer.h"

#ifdef _WIN32
#include <windows.h>

void get_console_size(int* w, int* h) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        *w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    } else {
        *w = 80; *h = 25;
    }
}

void move_cursor(int x, int y) {
    COORD pos = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), pos);
}

void hide_cursor(void) {
    CONSOLE_CURSOR_INFO ci = { 1, FALSE };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

void show_cursor(void) {
    CONSOLE_CURSOR_INFO ci = { 1, TRUE };
    SetConsoleCursorInfo(GetStdHandle(STD_OUTPUT_HANDLE), &ci);
}

void clear_screen(void) {
    system("cls");
}

#else
#include <sys/ioctl.h>
#include <unistd.h>

void get_console_size(int* w, int* h) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *w = ws.ws_col;
        *h = ws.ws_row;
    } else {
        *w = 80; *h = 25;
    }
}

void move_cursor(int x, int y) {
    printf("\033[%d;%dH", y + 1, x + 1);
}

void hide_cursor(void) {
    printf("\033[?25l");
    fflush(stdout);
}

void show_cursor(void) {
    printf("\033[?25h");
    fflush(stdout);
}

void clear_screen(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

#endif

/* ------------------------------------------------------------------ */
/* Simple FFT                                                          */
/* ------------------------------------------------------------------ */
static void fft(double* real, double* imag, int n) {
    int i, j, k, m, step;
    double tr, ti, ur, ui, wr, wi, angle;

    j = 0;
    for (i = 0; i < n - 1; i++) {
        if (i < j) {
            tr = real[i]; real[i] = real[j]; real[j] = tr;
            ti = imag[i]; imag[i] = imag[j]; imag[j] = ti;
        }
        k = n >> 1;
        while (k <= j) { j -= k; k >>= 1; }
        j += k;
    }

    for (step = 2; step <= n; step <<= 1) {
        int half = step >> 1;
        angle = -3.14159265358979323846 / half;
        wr = cos(angle);
        wi = sin(angle);
        for (i = 0; i < n; i += step) {
            ur = 1.0;
            ui = 0.0;
            for (m = 0; m < half; m++) {
                int a = i + m;
                int b = a + half;
                tr = ur * real[b] - ui * imag[b];
                ti = ur * imag[b] + ui * real[b];
                real[b] = real[a] - tr;
                imag[b] = imag[a] - ti;
                real[a] += tr;
                imag[a] += ti;
                double tmp = ur * wr - ui * wi;
                ui = ur * wi + ui * wr;
                ur = tmp;
            }
        }
    }
}

static double hann_window(int i, int n) {
    return 0.5 * (1.0 - cos(2.0 * 3.14159265358979323846 * i / (n - 1)));
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */
void vis_init(visualizer_t* vis, int sample_rate, int num_channels) {
    memset(vis, 0, sizeof(visualizer_t));
    vis->mode = VIS_VU_METERS;
    vis->sample_rate = sample_rate;
    vis->num_channels = num_channels;
    vis->fft_pos = 0;
    vis->update_interval = 2;  /* Update every 2 frames = ~90ms */
    vis->frame_counter = 0;

    if (num_channels > VIS_MAX_CHANNELS)
        vis->num_channels = VIS_MAX_CHANNELS;

    vis->num_bands = VIS_SPECTRUM_BANDS;
    double freq_min = 50.0;
    double freq_max = (double)sample_rate / 2.0;
    if (freq_max > 18000.0) freq_max = 18000.0;
    double log_min = log(freq_min);
    double log_max = log(freq_max);

    for (int i = 0; i < vis->num_bands; i++) {
        double lo = exp(log_min + (log_max - log_min) * i / vis->num_bands);
        double hi = exp(log_min + (log_max - log_min) * (i + 1) / vis->num_bands);
        vis->bands[i].freq_lo = lo;
        vis->bands[i].freq_hi = hi;
        vis->bands[i].magnitude = 0;
        vis->bands[i].decay = 0;
    }

    for (int i = 0; i < num_channels; i++) {
        vis->channels[i].peak = 0;
        vis->channels[i].rms = 0;
        vis->channels[i].decay_peak = 0;
        vis->channels[i].active = 1;
        snprintf(vis->channels[i].label, 32, "Ch%d", i + 1);
    }

    get_console_size(&vis->console_width, &vis->console_height);
    hide_cursor();
}

void vis_set_channel_label(visualizer_t* vis, int ch, const char* label) {
    if (ch >= 0 && ch < vis->num_channels) {
        strncpy(vis->channels[ch].label, label, 31);
        vis->channels[ch].label[31] = 0;
    }
}

void vis_feed_samples(visualizer_t* vis, int16_t* stereo_buf, int num_frames) {
    for (int i = 0; i < num_frames; i++) {
        /* Normalize to -1.0 .. 1.0 range */
        double sample = ((double)stereo_buf[i * 2] +
                         (double)stereo_buf[i * 2 + 1]) / 65536.0;
        vis->fft_input[vis->fft_pos] = sample;
        vis->fft_pos = (vis->fft_pos + 1) % VIS_FFT_SIZE;
    }
}

void vis_update_channel(visualizer_t* vis, int ch, double level, int active) {
    if (ch < 0 || ch >= vis->num_channels) return;

    /* level is already normalized 0..1 */
    double alpha = 0.3;
    vis->channels[ch].rms = vis->channels[ch].rms * (1.0 - alpha) + level * alpha;

    if (level > vis->channels[ch].peak)
        vis->channels[ch].peak = level;

    if (level > vis->channels[ch].decay_peak)
        vis->channels[ch].decay_peak = level;
    else
        vis->channels[ch].decay_peak *= 0.97;

    vis->channels[ch].active = active;
}

void vis_cycle_mode(visualizer_t* vis) {
    vis->mode = (vis->mode + 1) % VIS_COUNT;
    clear_screen();
    get_console_size(&vis->console_width, &vis->console_height);
}

const char* vis_mode_name(visualizer_t* vis) {
    switch (vis->mode) {
        case VIS_OFF: return "OFF";
        case VIS_VU_METERS: return "VU Meters";
        case VIS_SPECTRUM: return "Spectrum";
        default: return "?";
    }
}

/* ------------------------------------------------------------------ */
/* Bar rendering                                                       */
/* ------------------------------------------------------------------ */
static void render_bar(char* out, int max_width, double level, double peak_pos) {
    int bar_len = (int)(level * max_width);
    int peak_at = (int)(peak_pos * max_width);

    if (bar_len > max_width) bar_len = max_width;
    if (bar_len < 0) bar_len = 0;
    if (peak_at > max_width - 1) peak_at = max_width - 1;
    if (peak_at < 0) peak_at = 0;

    for (int i = 0; i < max_width; i++) {
        if (i < bar_len) {
            if (i > max_width * 3 / 4)
                out[i] = '#';
            else if (i > max_width / 2)
                out[i] = '=';
            else
                out[i] = '-';
        } else if (i == peak_at && peak_pos > 0.01) {
            out[i] = '|';
        } else {
            out[i] = ' ';
        }
    }
    out[max_width] = 0;
}

#ifdef _WIN32
static void print_colored_bar(int x, int y, const char* bar, int width) {
    HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
    COORD pos = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(hCon, pos);

    for (int i = 0; i < width && bar[i]; i++) {
        WORD attr;
        if (bar[i] == '#')
            attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
        else if (bar[i] == '=')
            attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        else if (bar[i] == '-')
            attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY;
        else if (bar[i] == '|')
            attr = FOREGROUND_RED | FOREGROUND_GREEN |
                   FOREGROUND_BLUE | FOREGROUND_INTENSITY;
        else
            attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

        SetConsoleTextAttribute(hCon, attr);
        putchar(bar[i]);
    }
    SetConsoleTextAttribute(hCon,
        FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
}
#else
static void print_colored_bar(int x, int y, const char* bar, int width) {
    move_cursor(x, y);
    for (int i = 0; i < width && bar[i]; i++) {
        if (bar[i] == '#')
            printf("\033[91m#\033[0m");
        else if (bar[i] == '=')
            printf("\033[93m=\033[0m");
        else if (bar[i] == '-')
            printf("\033[92m-\033[0m");
        else if (bar[i] == '|')
            printf("\033[97m|\033[0m");
        else
            putchar(' ');
    }
}
#endif

/* ------------------------------------------------------------------ */
/* VU meters                                                           */
/* ------------------------------------------------------------------ */
static void render_vu_meters(visualizer_t* vis, int start_line) {
    int bar_max = vis->console_width - 20;
    if (bar_max < 10) bar_max = 10;
    if (bar_max > 60) bar_max = 60;

    char bar[128];

    for (int i = 0; i < vis->num_channels; i++) {
        int y = start_line + i;
        if (y >= vis->console_height - 2) break;

        vu_channel_t* ch = &vis->channels[i];

        /* Scale for better visibility */
        double level = ch->rms * 1.5;
        double peak = ch->decay_peak * 1.5;
        if (level > 1.0) level = 1.0;
        if (peak > 1.0) peak = 1.0;

        if (!ch->active) {
            level = 0;
            peak = 0;
        }

        move_cursor(0, y);
        printf("%-10s ", ch->label);

        render_bar(bar, bar_max, level, peak);
        print_colored_bar(11, y, bar, bar_max);

        double db = -99.0;
        if (level > 0.001)
            db = 20.0 * log10(level);
        move_cursor(12 + bar_max, y);
        if (db > -99.0)
            printf(" %5.1fdB", db);
        else
            printf("  -inf ");
    }
}

/* ------------------------------------------------------------------ */
/* Spectrum analyzer with AGC                                          */
/* ------------------------------------------------------------------ */
static void render_spectrum(visualizer_t* vis, int start_line) {
    static double max_level = 0.1;
    static double gain = 1.0;

    for (int i = 0; i < VIS_FFT_SIZE; i++) {
        int idx = (vis->fft_pos + i) % VIS_FFT_SIZE;
        vis->fft_real[i] = vis->fft_input[idx] * hann_window(i, VIS_FFT_SIZE);
        vis->fft_imag[i] = 0;
    }
    fft(vis->fft_real, vis->fft_imag, VIS_FFT_SIZE);

    double bin_freq = (double)vis->sample_rate / VIS_FFT_SIZE;
    double current_max = 0;

    for (int b = 0; b < vis->num_bands; b++) {
        int bin_lo = (int)(vis->bands[b].freq_lo / bin_freq);
        int bin_hi = (int)(vis->bands[b].freq_hi / bin_freq);
        if (bin_lo < 1) bin_lo = 1;
        if (bin_hi >= VIS_FFT_SIZE / 2) bin_hi = VIS_FFT_SIZE / 2 - 1;
        if (bin_hi < bin_lo) bin_hi = bin_lo;

        double peak_mag = 0;
        for (int k = bin_lo; k <= bin_hi; k++) {
            double mag = sqrt(vis->fft_real[k] * vis->fft_real[k] +
                              vis->fft_imag[k] * vis->fft_imag[k]);
            if (mag > peak_mag) peak_mag = mag;
        }

        /* A-weighting curve to compensate for human hearing */
        double freq = (vis->bands[b].freq_lo + vis->bands[b].freq_hi) / 2;
        double a_weight = 1.0;
        if (freq < 1000) a_weight = freq / 1000.0;
        if (freq > 4000) a_weight = 4000.0 / freq;
        peak_mag *= a_weight;

        /* Apply AGC */
        peak_mag *= gain;

        if (peak_mag > current_max)
            current_max = peak_mag;

        double level = peak_mag;
        if (level > 1.0) level = 1.0;

        double alpha = 0.35;
        vis->bands[b].magnitude = vis->bands[b].magnitude * (1.0 - alpha) +
                                  level * alpha;

        if (level > vis->bands[b].decay)
            vis->bands[b].decay = level;
        else
            vis->bands[b].decay *= 0.94;
    }

    /* Automatic gain control */
    if (current_max > 0.01) {
        if (current_max > max_level)
            max_level = current_max;
        else
            max_level = max_level * 0.99 + current_max * 0.01;

        double target_gain = 0.9 / max_level;
        if (target_gain > 4.0) target_gain = 4.0;
        if (target_gain < 0.5) target_gain = 0.5;
        gain = gain * 0.95 + target_gain * 0.05;
    }

    /* Render */
    int max_height = vis->console_height - start_line - 3;
    if (max_height < 4) max_height = 4;
    if (max_height > 20) max_height = 20;

    int band_width = (vis->console_width - 4) / vis->num_bands;
    if (band_width < 1) band_width = 1;
    if (band_width > 4) band_width = 4;

    for (int row = 0; row < max_height; row++) {
        int y = start_line + row;
        if (y >= vis->console_height - 2) break;

        move_cursor(0, y);

        if (row == 0) printf("Hi ");
        else if (row == max_height - 1) printf("Lo ");
        else printf("   ");

        double threshold = 1.0 - (double)row / max_height;

        for (int b = 0; b < vis->num_bands; b++) {
            double mag = vis->bands[b].magnitude;
            double pk = vis->bands[b].decay;

            for (int w = 0; w < band_width; w++) {
                char c = ' ';
                int color_code = 0;

                if (mag >= threshold) {
                    c = '#';
                    if (threshold > 0.75) color_code = 3;
                    else if (threshold > 0.5) color_code = 2;
                    else color_code = 1;
                } else if (fabs(pk - threshold) < (1.0 / max_height)) {
                    c = '-';
                    color_code = 4;
                }

#ifdef _WIN32
                HANDLE hCon = GetStdHandle(STD_OUTPUT_HANDLE);
                WORD attr;
                switch (color_code) {
                    case 1: attr = FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
                    case 2: attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY; break;
                    case 3: attr = FOREGROUND_RED | FOREGROUND_INTENSITY; break;
                    case 4: attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY; break;
                    default: attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE; break;
                }
                SetConsoleTextAttribute(hCon, attr);
                putchar(c);
#else
                switch (color_code) {
                    case 1: printf("\033[92m%c\033[0m", c); break;
                    case 2: printf("\033[93m%c\033[0m", c); break;
                    case 3: printf("\033[91m%c\033[0m", c); break;
                    case 4: printf("\033[97m%c\033[0m", c); break;
                    default: putchar(c); break;
                }
#endif
            }
            putchar(' ');
        }

#ifdef _WIN32
        SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE),
            FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE);
#endif
    }

    /* Frequency labels */
    int y = start_line + max_height;
    if (y < vis->console_height - 1) {
        move_cursor(3, y);
        for (int b = 0; b < vis->num_bands; b++) {
            int freq = (int)vis->bands[b].freq_lo;
            char label[8];
            if (freq >= 1000)
                snprintf(label, sizeof(label), "%dk", freq / 1000);
            else
                snprintf(label, sizeof(label), "%d", freq);

            int pad = band_width + 1 - (int)strlen(label);
            if (pad < 0) pad = 0;
            printf("%s", label);
            for (int p = 0; p < pad; p++) putchar(' ');
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main render                                                         */
/* ------------------------------------------------------------------ */
void vis_render(visualizer_t* vis) {
    vis->frame_counter++;
    if (vis->frame_counter < vis->update_interval) return;
    vis->frame_counter = 0;

    if (vis->mode == VIS_OFF) return;

    get_console_size(&vis->console_width, &vis->console_height);

    int start_line = 4;

    switch (vis->mode) {
        case VIS_VU_METERS:
            render_vu_meters(vis, start_line);
            break;
        case VIS_SPECTRUM:
            render_spectrum(vis, start_line);
            break;
        default:
            break;
    }

    fflush(stdout);

    for (int i = 0; i < vis->num_channels; i++) {
        vis->channels[i].peak *= 0.95;
    }
}