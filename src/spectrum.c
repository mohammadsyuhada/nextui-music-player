#include "spectrum.h"
#include "player.h"
#include "defines.h"
#include "api.h"
#include "audio/kiss_fftr.h"
#include <math.h>
#include <string.h>

#define SMOOTHING_FACTOR 0.7f
#define PEAK_DECAY 0.97f
#define MIN_DB -60.0f
#define MAX_DB 0.0f

static kiss_fftr_cfg fft_cfg = NULL;
static kiss_fft_scalar fft_input[SPECTRUM_FFT_SIZE];
static kiss_fft_cpx fft_output[SPECTRUM_FFT_SIZE / 2 + 1];
static float hann_window[SPECTRUM_FFT_SIZE];
static float prev_bars[SPECTRUM_BARS];
static SpectrumData spectrum_data;
static int16_t sample_buffer[SPECTRUM_FFT_SIZE * 2];

static int bin_ranges[SPECTRUM_BARS + 1];

static int spec_x = 0, spec_y = 0, spec_w = 0, spec_h = 0;
static bool position_set = false;

static void init_hann_window(void) {
    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (SPECTRUM_FFT_SIZE - 1)));
    }
}

static void init_bin_ranges(void) {
    float min_freq = 80.0f;
    float max_freq = 16000.0f;
    float sample_rate = 48000.0f;
    float bin_resolution = sample_rate / SPECTRUM_FFT_SIZE;

    int min_bin = (int)(min_freq / bin_resolution);
    int max_bin = (int)(max_freq / bin_resolution);
    if (max_bin > SPECTRUM_FFT_SIZE / 2) max_bin = SPECTRUM_FFT_SIZE / 2;

    for (int i = 0; i <= SPECTRUM_BARS; i++) {
        float t = (float)i / SPECTRUM_BARS;
        float freq = min_freq * powf(max_freq / min_freq, t);
        int bin = (int)(freq / bin_resolution);
        if (bin < min_bin) bin = min_bin;
        if (bin > max_bin) bin = max_bin;
        bin_ranges[i] = bin;
    }
}

void Spectrum_init(void) {
    fft_cfg = kiss_fftr_alloc(SPECTRUM_FFT_SIZE, 0, NULL, NULL);
    init_hann_window();
    init_bin_ranges();
    memset(prev_bars, 0, sizeof(prev_bars));
    memset(&spectrum_data, 0, sizeof(spectrum_data));
}

void Spectrum_quit(void) {
    if (fft_cfg) {
        kiss_fftr_free(fft_cfg);
        fft_cfg = NULL;
    }
}

void Spectrum_update(void) {
    if (!fft_cfg) return;

    if (Player_getState() != PLAYER_STATE_PLAYING) {
        for (int i = 0; i < SPECTRUM_BARS; i++) {
            prev_bars[i] *= 0.9f;
            spectrum_data.bars[i] = prev_bars[i];
            spectrum_data.peaks[i] *= PEAK_DECAY;
        }
        spectrum_data.valid = true;
        return;
    }

    int samples = Player_getVisBuffer(sample_buffer, SPECTRUM_FFT_SIZE * 2);
    if (samples < SPECTRUM_FFT_SIZE) {
        spectrum_data.valid = false;
        return;
    }

    for (int i = 0; i < SPECTRUM_FFT_SIZE; i++) {
        float left = sample_buffer[i * 2];
        float right = sample_buffer[i * 2 + 1];
        float mono = (left + right) * 0.5f;
        fft_input[i] = (mono / 32768.0f) * hann_window[i];
    }

    kiss_fftr(fft_cfg, fft_input, fft_output);

    for (int i = 0; i < SPECTRUM_BARS; i++) {
        int start_bin = bin_ranges[i];
        int end_bin = bin_ranges[i + 1];
        if (end_bin <= start_bin) end_bin = start_bin + 1;

        float sum = 0.0f;
        int count = 0;
        for (int j = start_bin; j < end_bin && j < SPECTRUM_FFT_SIZE / 2 + 1; j++) {
            float re = fft_output[j].r;
            float im = fft_output[j].i;
            float mag = sqrtf(re * re + im * im);
            sum += mag;
            count++;
        }

        float avg_mag = (count > 0) ? sum / count : 0.0f;

        float db = 20.0f * log10f(avg_mag + 1e-10f);
        float normalized = (db - MIN_DB) / (MAX_DB - MIN_DB);
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        if (normalized > prev_bars[i]) {
            prev_bars[i] = normalized;
        } else {
            prev_bars[i] = prev_bars[i] * SMOOTHING_FACTOR + normalized * (1.0f - SMOOTHING_FACTOR);
        }

        spectrum_data.bars[i] = prev_bars[i];

        if (prev_bars[i] > spectrum_data.peaks[i]) {
            spectrum_data.peaks[i] = prev_bars[i];
        } else {
            spectrum_data.peaks[i] *= PEAK_DECAY;
        }
    }

    spectrum_data.valid = true;
}

const SpectrumData* Spectrum_getData(void) {
    return &spectrum_data;
}

void Spectrum_setPosition(int x, int y, int w, int h) {
    spec_x = x;
    spec_y = y;
    spec_w = w;
    spec_h = h;
    position_set = true;
}

bool Spectrum_needsRefresh(void) {
    return position_set && (Player_getState() == PLAYER_STATE_PLAYING);
}

void Spectrum_renderGPU(void) {
    if (!position_set) return;

    Spectrum_update();
    if (!spectrum_data.valid) return;

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0,
        spec_w, spec_h, 32, SDL_PIXELFORMAT_RGBA8888);
    if (!surface) return;

    SDL_FillRect(surface, NULL, 0);

    int total_bars = SPECTRUM_BARS;
    float bar_width_f = (float)spec_w / total_bars;
    int bar_gap = 1;
    int bar_draw_w = (int)bar_width_f - bar_gap;
    if (bar_draw_w < 1) bar_draw_w = 1;

    uint32_t white = SDL_MapRGBA(surface->format, 255, 255, 255, 255);

    for (int i = 0; i < total_bars; i++) {
        float magnitude = spectrum_data.bars[i];
        int bar_h = (int)(magnitude * spec_h * 0.9f);
        if (bar_h < 2) bar_h = 2;

        int bar_x_pos = (int)(i * bar_width_f);
        int bar_y_pos = spec_h - bar_h;

        SDL_Rect bar_rect = {bar_x_pos, bar_y_pos, bar_draw_w, bar_h};
        SDL_FillRect(surface, &bar_rect, white);

        if (spectrum_data.peaks[i] > magnitude + 0.02f) {
            int peak_y = spec_h - (int)(spectrum_data.peaks[i] * spec_h * 0.9f);
            SDL_Rect peak_rect = {bar_x_pos, peak_y, bar_draw_w, 2};
            SDL_FillRect(surface, &peak_rect, white);
        }
    }

    PLAT_clearLayers(LAYER_SPECTRUM);
    PLAT_drawOnLayer(surface, spec_x, spec_y, spec_w, spec_h, 1.0f, false, LAYER_SPECTRUM);
    SDL_FreeSurface(surface);

    PLAT_GPU_Flip();
}
