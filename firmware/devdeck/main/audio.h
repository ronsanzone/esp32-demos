#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool audio_mic_open(int sample_rate, float gain_db);   // ES7210 mics -> 16-bit stereo frames
int audio_mic_read(int16_t *stereo, int frames);       // blocking; returns frames read (0 on error)

#ifdef __cplusplus
}
#endif
