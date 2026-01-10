#ifndef __SPECTRUM_H__
#define __SPECTRUM_H__

#include <stdbool.h>

#define SPECTRUM_FFT_SIZE 512
#define SPECTRUM_BARS 64
#define LAYER_SPECTRUM 5

typedef struct {
    float bars[SPECTRUM_BARS];
    float peaks[SPECTRUM_BARS];
    bool valid;
} SpectrumData;

void Spectrum_init(void);
void Spectrum_quit(void);
void Spectrum_update(void);
const SpectrumData* Spectrum_getData(void);

void Spectrum_setPosition(int x, int y, int w, int h);
void Spectrum_renderGPU(void);
bool Spectrum_needsRefresh(void);

#endif
