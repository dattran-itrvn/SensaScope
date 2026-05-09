/*
 * SensaPulse — PDM stereo audio capture.
 *
 *  Channel mapping (per schematic + mic L/R config):
 *    ch0 = LEFT  = body mic    (MP23DB01HP, L/R=GND, falling edge)
 *    ch1 = RIGHT = ambient mic (IMP34DT05,  L/R=VDD, rising edge)
 *
 *  After #25 refactor: this module is a *producer*. It reads PDM blocks
 *  from the DMIC API and pushes the slab pointers into sd_writer's
 *  audio FIFO. It no longer touches FATFS directly. The actual
 *  audio.wav file is opened, written, finalized, and closed by
 *  sd_writer (single SD-writing thread).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_PDM_RATE_HZ   16000
#define AUDIO_PDM_CHANNELS  2
#define AUDIO_PDM_BIT_WIDTH 16

int audio_init(void);

/* ---------- One-shot capture (boot smoke test, blocking) ---------- */
int audio_record_to_wav(const char *path, int seconds,
			int32_t *peak_l, int32_t *peak_r,
			int32_t *mean_l, int32_t *mean_r);

/* ---------- Streaming producer (#25) ----------
 *
 *   audio_producer_start()    → configure DMIC, trigger START, spawn the
 *                                producer thread. Returns 0 on success.
 *
 *   audio_producer_stop()     → request producer thread exit; the thread
 *                                sets DMIC STOP and exits its loop. Use
 *                                audio_producer_is_running() to wait.
 *
 *   audio_producer_release_slab(buf) → called by sd_writer after it has
 *                                consumed a PDM block. Returns the slab
 *                                buffer to the mem-slab so the DMIC
 *                                driver can reuse it. Producers don't
 *                                free; consumer does, after fs_write.
 */
int  audio_producer_start(void);
int  audio_producer_stop(void);
bool audio_producer_is_running(void);
void audio_producer_release_slab(void *slab_buf);
