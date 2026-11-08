/*
 * ringkit — polyphase resampler (decimator and interpolator).
 *
 * Currently exposes only the 48k → 16k decimator (3:1) for the
 * STT → LLM → TTS pipeline. The 24k → 48k interpolator remains
 * private in ring_ws.c (its only consumer).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 48k → 16k decimator (D = 3) ---- */

#define RING_RESAMPLE_DECIM_PHASES     3
#define RING_RESAMPLE_DECIM_PHASE_LEN  21
#define RING_RESAMPLE_DECIM_HIST_LEN   (RING_RESAMPLE_DECIM_PHASES * RING_RESAMPLE_DECIM_PHASE_LEN - 1)  /* 62 */

typedef struct {
    float history[RING_RESAMPLE_DECIM_HIST_LEN];
} ring_resample_decim_state_t;

/** Initialise (zero the history buffer). */
void ring_resample_decim_init(ring_resample_decim_state_t *s);

/**
 * @brief Decimate 48 kHz float PCM → 16 kHz float PCM (exact 3:1).
 *
 * @param s      State (history) — use the same pointer across calls.
 * @param input  Input samples at 48 kHz; @p n_in must be a multiple of 3.
 * @param n_in   Number of input samples.
 * @param output Buffer for output; capacity must be at least n_in / 3.
 * @return Number of output samples written (n_in / 3).
 */
int ring_resample_decim_process(ring_resample_decim_state_t *s,
                                 const float *input, int n_in,
                                 float *output);

#ifdef __cplusplus
}
#endif