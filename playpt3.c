/* playpt3.c — unified PT3 player, audio in separate thread */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "ayumi.h"
#include "pt3player.h"
#include "load_text.h"
#include "visualizer.h"

#ifdef _WIN32
  #include <windows.h>
  #include <mmsystem.h>
  #include <conio.h>
  #include <process.h>
  #pragma comment(lib, "winmm.lib")
  #define PLATFORM_NAME "Windows"
#else
  #include <unistd.h>
  #include <signal.h>
  #include <termios.h>
  #include <fcntl.h>
  #include <sys/select.h>
  #include <pthread.h>
  #ifdef USE_PULSE
    #include <pulse/simple.h>
    #include <pulse/error.h>
    #define AUDIO_BACKEND_NAME "PulseAudio"
  #elif defined(USE_ALSA)
    #include <alsa/asoundlib.h>
    #define AUDIO_BACKEND_NAME "ALSA"
  #endif
  #define PLATFORM_NAME "Linux/" AUDIO_BACKEND_NAME
#endif

/* ================================================================== */
/* Cross-platform mutex                                                */
/* ================================================================== */
#ifdef _WIN32
  typedef CRITICAL_SECTION xmutex_t;
  static void xmutex_init(xmutex_t *m)    { InitializeCriticalSection(m); }
  static void xmutex_lock(xmutex_t *m)    { EnterCriticalSection(m); }
  static void xmutex_unlock(xmutex_t *m)  { LeaveCriticalSection(m); }
  static void xmutex_destroy(xmutex_t *m) { DeleteCriticalSection(m); }
#else
  typedef pthread_mutex_t xmutex_t;
  static void xmutex_init(xmutex_t *m)    { pthread_mutex_init(m, NULL); }
  static void xmutex_lock(xmutex_t *m)    { pthread_mutex_lock(m); }
  static void xmutex_unlock(xmutex_t *m)  { pthread_mutex_unlock(m); }
  static void xmutex_destroy(xmutex_t *m) { pthread_mutex_destroy(m); }
#endif

static xmutex_t audio_lock;

/* ================================================================== */
/* File loading                                                        */
/* ================================================================== */
static int load_file(const char *name, char **buffer, int *size) {
    FILE *f = fopen(name, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    *size = ftell(f);
    rewind(f);
    *buffer = (char *)malloc(*size + 1);
    if (!*buffer) { fclose(f); return 0; }
    if ((int)fread(*buffer, 1, *size, f) != *size) {
        free(*buffer); fclose(f); return 0;
    }
    fclose(f);
    (*buffer)[*size] = 0;
    return 1;
}

/* ================================================================== */
/* Channel map                                                         */
/* ================================================================== */
typedef enum {
    CHMAP_DEFAULT = 0, CHMAP_SPLIT_ALL, CHMAP_COMPACT,
    CHMAP_MINIMAL, CHMAP_BUZZER_SPLIT, CHMAP_COUNT
} channel_map_mode_t;

static const char *chmap_names[] = {
    "default","split-all","compact","minimal","buzzer-split"
};

#define MAX_VCHANNELS 40

typedef struct {
    int tone_mute[3]; int noise_mute[3]; int env_mute[3];
} chip_sub_mute_t;

static chip_sub_mute_t chip_mutes[10];

typedef struct {
    int chip, ay_channel, component, muted;
    char label[32];
} vchannel_t;

static vchannel_t vch[MAX_VCHANNELS];
static int num_vch = 0;
static channel_map_mode_t cur_chmap = CHMAP_DEFAULT;
static int buzzer_det[10][3];

static int is_buzzer(int chip, int ci, uint8_t *r) {
    uint16_t ep = (r[12] << 8) | r[11];
    int es = r[13];
    int he = (r[8 + ci] & 0x10) != 0;
    if (!he) { buzzer_det[chip][ci] = 0; return 0; }
    if (ep < 256 && es != 255 && es >= 8) {
        buzzer_det[chip][ci] = 1; return 1;
    }
    buzzer_det[chip][ci] = 0;
    return 0;
}

static void build_vch_map(int nc) {
    int idx = 0;
    const char *cn[] = {"A","B","C"};
    memset(vch, 0, sizeof(vch));
    memset(chip_mutes, 0, sizeof(chip_mutes));

    switch (cur_chmap) {
    case CHMAP_DEFAULT:
        for (int c = 0; c < nc; c++)
            for (int i = 0; i < 3; i++) {
                if (nc > 1) snprintf(vch[idx].label, 32, "C%d-%s", c+1, cn[i]);
                else snprintf(vch[idx].label, 32, "%s", cn[i]);
                vch[idx].chip = c; vch[idx].ay_channel = i;
                vch[idx].component = 3;
                idx++; if (idx >= MAX_VCHANNELS) goto done;
            }
        break;
    case CHMAP_SPLIT_ALL: {
        const char *cc[] = {"Tone","Noise","Env"};
        for (int c = 0; c < nc; c++)
            for (int i = 0; i < 3; i++)
                for (int p = 0; p < 3; p++) {
                    if (nc > 1) snprintf(vch[idx].label, 32, "C%d-%s-%s", c+1, cn[i], cc[p]);
                    else snprintf(vch[idx].label, 32, "%s-%s", cn[i], cc[p]);
                    vch[idx].chip = c; vch[idx].ay_channel = i;
                    vch[idx].component = p;
                    idx++; if (idx >= MAX_VCHANNELS) goto done;
                }
        break;
    }
    case CHMAP_COMPACT: {
        const char *gn[] = {"Tones","Noise","Envelopes"};
        for (int c = 0; c < nc; c++)
            for (int p = 0; p < 3; p++) {
                if (nc > 1) snprintf(vch[idx].label, 32, "C%d-%s", c+1, gn[p]);
                else snprintf(vch[idx].label, 32, "%s", gn[p]);
                vch[idx].chip = c; vch[idx].ay_channel = -1;
                vch[idx].component = p;
                idx++; if (idx >= MAX_VCHANNELS) goto done;
            }
        break;
    }
    case CHMAP_MINIMAL: {
        const char *gn[] = {"Melody","Bass/FX","Noise"};
        int gc[] = {0,2,1};
        for (int c = 0; c < nc; c++)
            for (int g = 0; g < 3; g++) {
                if (nc > 1) snprintf(vch[idx].label, 32, "C%d-%s", c+1, gn[g]);
                else snprintf(vch[idx].label, 32, "%s", gn[g]);
                vch[idx].chip = c; vch[idx].ay_channel = -1;
                vch[idx].component = gc[g];
                idx++; if (idx >= MAX_VCHANNELS) goto done;
            }
        break;
    }
    case CHMAP_BUZZER_SPLIT: {
        const char *gn[] = {"Normal","Buzzer","Noise","Envelope"};
        int gc[] = {0,4,1,2};
        for (int c = 0; c < nc; c++)
            for (int g = 0; g < 4; g++) {
                if (nc > 1) snprintf(vch[idx].label, 32, "C%d-%s", c+1, gn[g]);
                else snprintf(vch[idx].label, 32, "%s", gn[g]);
                vch[idx].chip = c; vch[idx].ay_channel = -1;
                vch[idx].component = gc[g];
                idx++; if (idx >= MAX_VCHANNELS) goto done;
            }
        break;
    }
    default: break;
    }
done:
    num_vch = idx;
}

static void toggle_vch(int i) {
    if (i >= 0 && i < num_vch)
        vch[i].muted = !vch[i].muted;
}

static void apply_mutes(int chip, uint8_t *r) {
    chip_sub_mute_t *m = &chip_mutes[chip];
    memset(m, 0, sizeof(*m));
    for (int v = 0; v < num_vch; v++) {
        if (vch[v].chip != chip || !vch[v].muted) continue;
        switch (cur_chmap) {
        case CHMAP_DEFAULT:
            if (vch[v].ay_channel >= 0 && vch[v].ay_channel < 3) {
                int c = vch[v].ay_channel;
                m->tone_mute[c] = m->noise_mute[c] = m->env_mute[c] = 1;
            }
            break;
        case CHMAP_SPLIT_ALL:
            if (vch[v].ay_channel >= 0 && vch[v].ay_channel < 3) {
                int c = vch[v].ay_channel;
                switch (vch[v].component) {
                    case 0: m->tone_mute[c] = 1; break;
                    case 1: m->noise_mute[c] = 1; break;
                    case 2: m->env_mute[c] = 1; break;
                }
            }
            break;
        case CHMAP_COMPACT:
            for (int c = 0; c < 3; c++)
                switch (vch[v].component) {
                    case 0: m->tone_mute[c] = 1; break;
                    case 1: m->noise_mute[c] = 1; break;
                    case 2: m->env_mute[c] = 1; break;
                }
            break;
        case CHMAP_MINIMAL:
            if (!r) break;
            for (int c = 0; c < 3; c++) {
                int he = (r[8+c] & 0x10) != 0;
                switch (vch[v].component) {
                    case 0: if (!he) m->tone_mute[c] = 1; break;
                    case 2: if (he) { m->tone_mute[c] = 1; m->env_mute[c] = 1; } break;
                    case 1: m->noise_mute[c] = 1; break;
                }
            }
            break;
        case CHMAP_BUZZER_SPLIT:
            if (!r) break;
            for (int c = 0; c < 3; c++) {
                int ib = is_buzzer(chip, c, r);
                int he = (r[8+c] & 0x10) != 0;
                switch (vch[v].component) {
                    case 0: if (!ib) m->tone_mute[c] = 1; break;
                    case 4: if (ib) { m->tone_mute[c] = 1; m->env_mute[c] = 1; } break;
                    case 1: m->noise_mute[c] = 1; break;
                    case 2: if (!ib && he) m->env_mute[c] = 1; break;
                }
            }
            break;
        default: break;
        }
    }
}

static void print_ch_status(void) {
    move_cursor(0, 2);
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(h, &csbi);
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        COORD pos = {0, 2};
        SetConsoleCursorPosition(h, pos);
        for (int i = 0; i < w - 1; i++) putchar(' ');
        SetConsoleCursorPosition(h, pos);
    }
#else
    printf("\033[K");
#endif
    printf("[%s] ", chmap_names[cur_chmap]);
    for (int i = 0; i < num_vch; i++) {
        char key = (i < 9) ? ('1' + i) : ('a' + (i - 9));
        printf("%c:%s=%s ", key, vch[i].label,
               vch[i].muted ? "OFF" : "on");
    }
    fflush(stdout);
}



/* ================================================================== */
/* Global state                                                        */
/* ================================================================== */
static unsigned char ayreg[14];
static struct ayumi ay[10];
static int numchips = 0;
static int volume = 10000;
static volatile int paused_flag = 0;
static struct ay_data t;
static char files[10][255];
static int numfiles = 0;

#define CHUNK_FRAMES 882  /* 20ms at 44100Hz */

static int16_t *tmpbuf[10];
static volatile int frame_cnt[10];
static int samp_pos[10];
static volatile int fast = 0;
static char *music_buf;
static int music_size;
static visualizer_t vis;

/* Shared spectrum buffer: audio thread writes, GUI thread reads */
#define SPEC_BUF_SIZE 2048
static int16_t spec_buf[SPEC_BUF_SIZE];
static volatile int spec_buf_ready = 0;
static int spec_buf_len = 0;

static volatile int running = 1;
static volatile int rewind_request = 0;
static volatile int rewind_target = 0;

static int loadfiles(int first);

/* ================================================================== */
/* Terminal                                                            */
/* ================================================================== */
#ifndef _WIN32
static struct termios orig_termios;
static int raw_active = 0;

static void disable_raw(void) {
    if (raw_active) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_active = 0;
    }
    printf("\033[?25h");
    fflush(stdout);
}

static void enable_raw(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &orig_termios);
    raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    raw_active = 1;
    atexit(disable_raw);
}

static int kbhit_l(void) {
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int getch_l(void) {
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? (int)c : -1;
}
#endif

/*
static void print_header(void) {
    move_cursor(0, 0);
#ifdef _WIN32
    {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        GetConsoleScreenBufferInfo(h, &csbi);
        int w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        COORD pos = {0, 0};
        SetConsoleCursorPosition(h, pos);
        for (int i = 0; i < w - 1; i++) putchar(' ');
        SetConsoleCursorPosition(h, pos);
    }
#else
    printf("\033[K");
#endif
    printf("PT3 Player [%s] vis:%s  (v=vis m=chmap)",
           PLATFORM_NAME, vis_mode_name(&vis));
    fflush(stdout);
}
*/

static void print_header(void) {
    move_cursor(0,0);
#ifdef _WIN32
    printf("PT3 Player [%s] vis:%s  (v=vis m=chmap)          ",PLATFORM_NAME,vis_mode_name(&vis));
#else
    printf("\033[KPT3 Player [%s] vis:%s  (v=vis m=chmap)",PLATFORM_NAME,vis_mode_name(&vis));
#endif
    fflush(stdout);
}


/* ================================================================== */
/* Key codes                                                           */
/* ================================================================== */
#define K_NONE  0
#define K_ESC   27
#define K_LEFT  1001
#define K_RIGHT 1002
#define K_HOME  1003
#define K_PGUP  1004
#define K_PGDN  1005

static int read_key(void) {
#ifdef _WIN32
    if (!_kbhit()) return K_NONE;
    int c = _getch();
    if (c == 27) return K_ESC;
    if (c == 0 || c == 224) {
        if (!_kbhit()) return K_NONE;
        c = _getch();
        switch (c) {
            case 73: return K_PGUP;
            case 81: return K_PGDN;
            case 71: return K_HOME;
            case 75: return K_LEFT;
            case 77: return K_RIGHT;
        }
        return K_NONE;
    }
    return c;
#else
    if (!kbhit_l()) return K_NONE;
    int c = getch_l();
    if (c < 0) return K_NONE;
    if (c == 27) {
        usleep(10000);
        if (!kbhit_l()) return K_ESC;
        int c2 = getch_l();
        if (c2 < 0) return K_ESC;
        if (c2 == '[') {
            int c3 = getch_l();
            if (c3 < 0) return K_ESC;
            switch (c3) {
                case 'D': return K_LEFT;
                case 'C': return K_RIGHT;
                case 'H': return K_HOME;
                case '5': getch_l(); return K_PGUP;
                case '6': getch_l(); return K_PGDN;
                case '1': getch_l(); return K_HOME;
                default: return K_NONE;
            }
        }
        if (c2 == 'O') {
            int c3 = getch_l();
            if (c3 == 'H') return K_HOME;
            return K_NONE;
        }
        return K_ESC;
    }
    return c;
#endif
}

#ifndef _WIN32
static void sig_handler(int s) { (void)s; running = 0; }
#endif

/* ================================================================== */
/* AY update                                                           */
/* ================================================================== */
static void update_ay(struct ayumi *a, uint8_t *r, int ch) {
    func_getregs(r, ch);
    apply_mutes(ch, r);
    chip_sub_mute_t *m = &chip_mutes[ch];

    ayumi_set_tone(a, 0, (r[1] << 8) | r[0]);
    ayumi_set_tone(a, 1, (r[3] << 8) | r[2]);
    ayumi_set_tone(a, 2, (r[5] << 8) | r[4]);
    ayumi_set_noise(a, r[6]);

    for (int i = 0; i < 3; i++) {
        int to = (r[7] >> i) & 1;
        int no = (r[7] >> (i + 3)) & 1;
        int eo = (r[8 + i] >> 4) & 1;
        int vol = r[8 + i] & 0xf;
        if (m->tone_mute[i]) to = 1;
        if (m->noise_mute[i]) no = 1;
        if (m->env_mute[i] && eo) { eo = 0; vol = 0; }
        if (m->tone_mute[i] && m->noise_mute[i]) { vol = 0; eo = 0; }
        ayumi_set_mixer(a, i, to, no, eo);
        ayumi_set_volume(a, i, vol);
    }

    ayumi_set_envelope(a, (r[12] << 8) | r[11]);
    if (r[13] != 255) ayumi_set_envelope_shape(a, r[13]);
}

/* ================================================================== */
/* Render                                                              */
/* ================================================================== */
static void renday(int16_t *buf, int nf, struct ayumi *a,
                   struct ay_data *td, int ch) {
    int step = (int)(td->sample_rate / td->frame_rate);
    if (fast && step > 4) step /= 4;

    for (int i = 0; i < nf; i++) {
        if (!paused_flag) {
            if (samp_pos[ch] >= step) {
                func_play_tick(ch);
                update_ay(a, ayreg, ch);
                samp_pos[ch] = 0;
                frame_cnt[ch]++;

                /* VU update */
                for (int ac = 0; ac < 3; ac++) {
                    int vr = ayreg[8 + ac] & 0x0f;
                    int he = (ayreg[8 + ac] & 0x10) != 0;
                    double lvl = he ? 0.8 : (double)vr / 15.0;
                    for (int v = 0; v < num_vch; v++) {
                        if (vch[v].chip != ch) continue;
                        if (cur_chmap == CHMAP_DEFAULT) {
                            if (vch[v].ay_channel == ac)
                                vis_update_channel(&vis, v,
                                    vch[v].muted ? 0 : lvl,
                                    !vch[v].muted);
                        } else if (vch[v].ay_channel == ac) {
                            double l2 = lvl;
                            if (vch[v].component == 1) l2 *= 0.5;
                            else if (vch[v].component == 2)
                                l2 = he ? lvl : 0;
                            else l2 *= 0.7;
                            vis_update_channel(&vis, v,
                                vch[v].muted ? 0 : l2, 1);
                        }
                    }
                }
            }
            ayumi_process(a);
            if (td->dc_filter_on) ayumi_remove_dc(a);
            buf[i * 2]     = (int16_t)(a->left  * volume);
            buf[i * 2 + 1] = (int16_t)(a->right * volume);
        } else {
            buf[i * 2] = 0;
            buf[i * 2 + 1] = 0;
        }
        samp_pos[ch]++;
    }
}

static void do_rewind(int skip) {
    if (skip < 0) skip = 0;
    for (int c = 0; c < numchips; c++) {
        func_restart_music(c);
        frame_cnt[c] = 0;
        samp_pos[c] = 0;
    }
    for (int i = 0; i < skip; i++)
        for (int c = 0; c < numchips; c++) {
            func_play_tick(c);
            frame_cnt[c]++;
        }
}

static void setup_vis(void) {
    vis_init(&vis, t.sample_rate, num_vch);
    for (int i = 0; i < num_vch; i++)
        vis_set_channel_label(&vis, i, vch[i].label);
}

static void render_mix(int16_t *out, int nf) {
    for (int c = 0; c < numchips; c++)
        renday(tmpbuf[c], nf, &ay[c], &t, c);
    for (int j = 0; j < nf * 2; j++) {
        int32_t v = 0;
        for (int c = 0; c < numchips; c++) v += tmpbuf[c][j];
        if (numchips > 0) v /= numchips;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        out[j] = (int16_t)v;
    }
}

/* ================================================================== */
/* Audio thread                                                        */
/* ================================================================== */
#ifdef _WIN32

/* Windows: waveOut with callback, audio thread fills buffers */
#define WIN_NUM_BUFS 4
//#define CHUNK_FRAMES 441
#define WIN_BUF_FRAMES CHUNK_FRAMES

static HWAVEOUT hWaveOut;
static WAVEHDR wHdr[WIN_NUM_BUFS];
static int16_t wBuf[WIN_NUM_BUFS][WIN_BUF_FRAMES * 2];
static HANDLE hBufEvent;

static void CALLBACK waveCallback(HWAVEOUT hwo, UINT uMsg,
    DWORD_PTR dwInstance, DWORD_PTR dwParam1, DWORD_PTR dwParam2) {
    (void)hwo; (void)dwInstance; (void)dwParam2;
    if (uMsg == WOM_DONE)
        SetEvent(hBufEvent);
}

static unsigned __stdcall audio_thread_func(void *arg) {
    (void)arg;
    WAVEFORMATEX wfx;
    memset(&wfx, 0, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = 2;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = 4;
    wfx.nSamplesPerSec = t.sample_rate;
    wfx.nAvgBytesPerSec = t.sample_rate * 4;

    hBufEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    if (waveOutOpen(&hWaveOut, WAVE_MAPPER, &wfx,
        (DWORD_PTR)waveCallback, 0, CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        running = 0;
        return 1;
    }

    for (int i = 0; i < WIN_NUM_BUFS; i++) {
        memset(&wHdr[i], 0, sizeof(WAVEHDR));
        wHdr[i].lpData = (LPSTR)wBuf[i];
        wHdr[i].dwBufferLength = WIN_BUF_FRAMES * 4;
        waveOutPrepareHeader(hWaveOut, &wHdr[i], sizeof(WAVEHDR));
    }

    /* Pre-fill and submit all buffers */
    for (int i = 0; i < WIN_NUM_BUFS; i++) {
        xmutex_lock(&audio_lock);
        render_mix(wBuf[i], WIN_BUF_FRAMES);
        xmutex_unlock(&audio_lock);
        waveOutWrite(hWaveOut, &wHdr[i], sizeof(WAVEHDR));
    }

    int cur = 0;

    while (running) {
        /* Wait for a buffer to finish playing */
        WaitForSingleObject(hBufEvent, 20);

        /* Check for rewind request */
        if (rewind_request) {
            waveOutReset(hWaveOut);
            xmutex_lock(&audio_lock);
            do_rewind(rewind_target);
            xmutex_unlock(&audio_lock);
            rewind_request = 0;

            /* Re-fill and re-submit all buffers */
            for (int i = 0; i < WIN_NUM_BUFS; i++) {
                xmutex_lock(&audio_lock);
                render_mix(wBuf[i], WIN_BUF_FRAMES);
                xmutex_unlock(&audio_lock);
                wHdr[i].dwFlags &= ~WHDR_DONE;
                waveOutWrite(hWaveOut, &wHdr[i], sizeof(WAVEHDR));
            }
            cur = 0;
            continue;
        }

        /* Fill any completed buffers */
        for (int i = 0; i < WIN_NUM_BUFS; i++) {
            int bi = (cur + i) % WIN_NUM_BUFS;
            if (wHdr[bi].dwFlags & WHDR_DONE) {
                xmutex_lock(&audio_lock);
                render_mix(wBuf[bi], WIN_BUF_FRAMES);

                /* Copy a portion for spectrum analysis */
                if (!spec_buf_ready) {
                    int copy = WIN_BUF_FRAMES * 2;
                    if (copy > SPEC_BUF_SIZE) copy = SPEC_BUF_SIZE;
                    memcpy(spec_buf, wBuf[bi], copy * sizeof(int16_t));
                    spec_buf_len = copy / 2;
                    spec_buf_ready = 1;
                }

                xmutex_unlock(&audio_lock);
                wHdr[bi].dwFlags &= ~WHDR_DONE;
                waveOutWrite(hWaveOut, &wHdr[bi], sizeof(WAVEHDR));
                cur = (bi + 1) % WIN_NUM_BUFS;
            }
        }
    }

    waveOutReset(hWaveOut);
    for (int i = 0; i < WIN_NUM_BUFS; i++)
        waveOutUnprepareHeader(hWaveOut, &wHdr[i], sizeof(WAVEHDR));
    waveOutClose(hWaveOut);
    CloseHandle(hBufEvent);
    return 0;
}

#else /* Linux audio thread */

#ifdef USE_PULSE
static pa_simple *pa_h = NULL;
#elif defined(USE_ALSA)
static snd_pcm_t *alsa_h = NULL;
#endif

static void *audio_thread_func(void *arg) {
    (void)arg;
    int16_t lbuf[CHUNK_FRAMES * 2];

#ifdef USE_PULSE
    {
        pa_sample_spec ss;
        int err;
        ss.format = PA_SAMPLE_S16LE;
        ss.channels = 2;
        ss.rate = t.sample_rate;
        pa_h = pa_simple_new(NULL, "playpt3", PA_STREAM_PLAYBACK,
                             NULL, "PT3", &ss, NULL, NULL, &err);
        if (!pa_h) {
            fprintf(stderr, "PA: %s\n", pa_strerror(err));
            running = 0;
            return NULL;
        }
    }
#elif defined(USE_ALSA)
    {
        int err;
        snd_pcm_hw_params_t *p;
        unsigned int rate = t.sample_rate;
        snd_pcm_uframes_t per = 512;
        if ((err = snd_pcm_open(&alsa_h, "default",
             SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
            fprintf(stderr, "ALSA: %s\n", snd_strerror(err));
            running = 0;
            return NULL;
        }
        snd_pcm_hw_params_alloca(&p);
        snd_pcm_hw_params_any(alsa_h, p);
        snd_pcm_hw_params_set_access(alsa_h, p,
            SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format(alsa_h, p, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_channels(alsa_h, p, 2);
        snd_pcm_hw_params_set_rate_near(alsa_h, p, &rate, 0);
        snd_pcm_hw_params_set_period_size_near(alsa_h, p, &per, 0);
        if ((err = snd_pcm_hw_params(alsa_h, p)) < 0) {
            fprintf(stderr, "ALSA: %s\n", snd_strerror(err));
            snd_pcm_close(alsa_h);
            alsa_h = NULL;
            running = 0;
            return NULL;
        }
        snd_pcm_prepare(alsa_h);
    }
#endif

    while (running) {
        if (rewind_request) {
            xmutex_lock(&audio_lock);
            do_rewind(rewind_target);
            xmutex_unlock(&audio_lock);
            rewind_request = 0;
        }

        xmutex_lock(&audio_lock);
        render_mix(lbuf, CHUNK_FRAMES);

        if (!spec_buf_ready) {
            int copy = CHUNK_FRAMES * 2;
            if (copy > SPEC_BUF_SIZE) copy = SPEC_BUF_SIZE;
            memcpy(spec_buf, lbuf, copy * sizeof(int16_t));
            spec_buf_len = copy / 2;
            spec_buf_ready = 1;
        }

        xmutex_unlock(&audio_lock);

#ifdef USE_PULSE
        {
            int err;
            if (pa_simple_write(pa_h, lbuf, CHUNK_FRAMES * 4, &err) < 0) {
                fprintf(stderr, "PA write: %s\n", pa_strerror(err));
                break;
            }
        }
#elif defined(USE_ALSA)
        {
            int nf = CHUNK_FRAMES;
            int16_t *ptr = lbuf;
            while (nf > 0) {
                int w = snd_pcm_writei(alsa_h, ptr, nf);
                if (w < 0) {
                    w = snd_pcm_recover(alsa_h, w, 0);
                    if (w < 0) break;
                }
                nf -= w;
                ptr += w * 2;
            }
        }
#endif
    }

#ifdef USE_PULSE
    if (pa_h) { pa_simple_drain(pa_h, NULL); pa_simple_free(pa_h); pa_h = NULL; }
#elif defined(USE_ALSA)
    if (alsa_h) { snd_pcm_drain(alsa_h); snd_pcm_close(alsa_h); alsa_h = NULL; }
#endif

    return NULL;
}
#endif

/* ================================================================== */
/* Key handling                                                        */
/* ================================================================== */
static int handle_key(int key) {
    int need_st = 0;
    switch (key) {
    case K_ESC:
        running = 0;
        return 1;
    case 'f': case 'F':
        fast = !fast;
        break;
    case ' ':
        paused_flag = !paused_flag;
        break;
    case 'v': case 'V':
        vis_cycle_mode(&vis);
        print_header();
        print_ch_status();
        break;
    case 'r': case 'R': {
        int cp = frame_cnt[0] - 25;
        if (cp < 0) cp = 0;
        xmutex_lock(&audio_lock);
        loadfiles(0);
        build_vch_map(numchips);
        setup_vis();
        do_rewind(cp);
        xmutex_unlock(&audio_lock);
        clear_screen();
        print_header();
        need_st = 1;
        break;
    }
    case K_PGUP:
        volume = (int)(volume * 1.1);
        if (volume > 15000) volume = 15000;
        break;
    case K_PGDN:
        volume = (int)(volume / 1.1);
        if (volume < 10) volume = 10;
        break;
    case K_HOME:
        rewind_target = 0;
        rewind_request = 1;
        break;
    case K_LEFT: {
        int sk = frame_cnt[0] - 100;
        if (sk < 0) sk = 0;
        rewind_target = sk;
        rewind_request = 1;
        break;
    }
    case K_RIGHT:
        rewind_target = frame_cnt[0] + 100;
        rewind_request = 1;
        break;
    case 'm': case 'M':
        xmutex_lock(&audio_lock);
        cur_chmap = (cur_chmap + 1) % CHMAP_COUNT;
        build_vch_map(numchips);
        setup_vis();
        xmutex_unlock(&audio_lock);
        clear_screen();
        print_header();
        need_st = 1;
        break;
    case '0':
        for (int v = 0; v < num_vch; v++) vch[v].muted = 0;
        memset(chip_mutes, 0, sizeof(chip_mutes));
        need_st = 1;
        break;
    default:
        if (key >= '1' && key <= '9') {
            int idx = key - '1';
            if (idx < num_vch) { toggle_vch(idx); need_st = 1; }
        } else if (key >= 'a' && key <= 'z'
                   && key != 'f' && key != 'm'
                   && key != 'r' && key != 'v') {
            int idx = 9;
            for (int c = 'a'; c < key; c++)
                if (c != 'f' && c != 'm' && c != 'r' && c != 'v')
                    idx++;
            if (idx < num_vch) { toggle_vch(idx); need_st = 1; }
        }
        break;
    }
    if (need_st) print_ch_status();
    return 0;
}

/* ================================================================== */
/* Main play loop — GUI thread                                         */
/* ================================================================== */
static void play(void) {
    build_vch_map(numchips);
    setup_vis();
    clear_screen();
    print_header();
    print_ch_status();

    xmutex_init(&audio_lock);

    /* Start audio thread */
#ifdef _WIN32
    HANDLE hThread = (HANDLE)_beginthreadex(NULL, 0,
        audio_thread_func, NULL, 0, NULL);
#else
    pthread_t tid;
    pthread_create(&tid, NULL, audio_thread_func, NULL);
#endif

    /* GUI loop: display + keyboard, completely independent of audio */
    while (running) {
#ifdef _WIN32
        Sleep(10);
#else
        usleep(10000);
#endif
        /* Status line */
        move_cursor(0, 1);
#ifdef _WIN32
        printf("pos=%-6d vol=%-3d%% %s%s vis:%-10s      ",
#else
        printf("\033[Kpos=%-6d vol=%-3d%% %s%s vis:%-10s",
#endif
               frame_cnt[0], volume / 150,
               paused_flag ? "PAUSED " : "",
               fast ? "FAST " : "",
               vis_mode_name(&vis));
        fflush(stdout);

        /* Feed spectrum from shared buffer */
        if (spec_buf_ready) {
            vis_feed_samples(&vis, spec_buf, spec_buf_len);
            spec_buf_ready = 0;
        }

        vis_render(&vis);

        /* Read keys */
        int key = read_key();
        if (key != K_NONE) {
            if (handle_key(key)) break;
        }
    }

    /* Wait for audio thread to finish */
#ifdef _WIN32
    WaitForSingleObject(hThread, 2000);
    CloseHandle(hThread);
#else
    pthread_join(tid, NULL);
#endif

    xmutex_destroy(&audio_lock);
    show_cursor();
    printf("\n");
}

/* ================================================================== */
/* Defaults & loading                                                  */
/* ================================================================== */
static void set_defaults(struct ay_data *td) {
    memset(td, 0, sizeof(*td));
    td->sample_rate = 44100;
    td->eqp_stereo_on = 1;
    td->dc_filter_on = 1;
    td->is_ym = 1;
    td->pan[0] = 0.1;
    td->pan[1] = 0.5;
    td->pan[2] = 0.9;
    td->clock_rate = 1750000;
    td->frame_rate = 50;
    td->note_table = -1;
}

static int loadfiles(int first) {
    load_text_file("playpt3.txt", &t);
    forced_notetable = t.note_table;
    numchips = 0;
    for (int fn = 0; fn < numfiles; fn++) {
        if (!load_file(files[fn], &music_buf, &music_size)) {
            printf("Load error: %s\n", files[fn]);
            return 0;
        }
        if (first)
            printf("*** Loaded \"%s\" %d bytes\n", files[fn], music_size);
        int num = func_setup_music((uint8_t *)music_buf, music_size,
                                    numchips, first);
        if (num <= 0) { printf("Setup error\n"); return 0; }
        numchips += num;
        if (first) printf("Chips: %d\n", num);
    }
    if (numchips <= 0) { printf("No chips\n"); return 0; }
    if (first) printf("Total chips: %d\n", numchips);

    int as = CHUNK_FRAMES * 2 * (int)sizeof(int16_t);
    for (int c = 0; c < numchips; c++) {
        if (!ayumi_configure(&ay[c], t.is_ym, t.clock_rate,
                              t.sample_rate)) {
            printf("ayumi error\n");
            return 0;
        }
        ayumi_set_pan(&ay[c], 0, t.pan[0], t.eqp_stereo_on);
        ayumi_set_pan(&ay[c], 1, t.pan[1], t.eqp_stereo_on);
        ayumi_set_pan(&ay[c], 2, t.pan[2], t.eqp_stereo_on);
        if (!tmpbuf[c])
            tmpbuf[c] = (int16_t *)malloc(as);
        else
            tmpbuf[c] = (int16_t *)realloc(tmpbuf[c], as);
        if (!tmpbuf[c]) { printf("malloc\n"); return 0; }
        memset(tmpbuf[c], 0, as);
        frame_cnt[c] = 0;
        samp_pos[c] = 0;
        if (first) printf("Ayumi #%d ok\n", c);
    }
    paused_flag = 0;
    memset(buzzer_det, 0, sizeof(buzzer_det));
    return 1;
}

static channel_map_mode_t parse_chmap(const char *name) {
    for (int i = 0; i < CHMAP_COUNT; i++)
        if (strcmp(name, chmap_names[i]) == 0)
            return (channel_map_mode_t)i;
    fprintf(stderr, "Unknown chmap '%s'\n", name);
    return CHMAP_DEFAULT;
}

/* ================================================================== */
/* main                                                                */
/* ================================================================== */
int main(int argc, char **argv) {
    if (argc < 2) {
        printf(
            "Usage: playpt3 [--channel-map MODE] <file.pt3> ...\n\n"
            "  v           - cycle visualizer (OFF/VU/Spectrum)\n"
            "  m           - cycle channel-map\n"
            "  1-9,a-z     - toggle channels\n"
            "  0           - unmute all\n"
            "  f           - fast x4\n"
            "  space       - pause\n"
            "  pgup/pgdn   - volume\n"
            "  left/right  - seek +-2s\n"
            "  home        - restart\n"
            "  r           - reload\n"
            "  esc         - exit\n\n"
            "Channel maps: default, split-all, compact, "
            "minimal, buzzer-split\n");
        return 1;
    }

#ifndef _WIN32
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
#endif

    set_defaults(&t);
    memset(tmpbuf, 0, sizeof(tmpbuf));
    memset(buzzer_det, 0, sizeof(buzzer_det));
    numfiles = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--channel-map") == 0 && i + 1 < argc) {
            cur_chmap = parse_chmap(argv[++i]);
        } else if (argv[i][0] == '-' && argv[i][1] == '-') {
            fprintf(stderr, "Unknown: %s\n", argv[i]);
        } else if (numfiles < 10) {
            strncpy(files[numfiles], argv[i], 254);
            files[numfiles][254] = 0;
            numfiles++;
        }
    }
    if (!numfiles) { printf("No files\n"); return 1; }

#ifndef _WIN32
    enable_raw();
#endif

    if (loadfiles(1)) {
        play();
        printf("Finished\n");
    } else {
        printf("Failed\n");
    }

#ifndef _WIN32
    disable_raw();
#endif

    for (int c = 0; c < 10; c++)
        if (tmpbuf[c]) { free(tmpbuf[c]); tmpbuf[c] = NULL; }
    return 0;
}