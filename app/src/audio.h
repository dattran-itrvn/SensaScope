/*
 * SensaPulse — PDM stereo audio capture.
 *
 *  Channel mapping (per schematic + mic L/R config):
 *    ch0 = LEFT  = body mic    (MP23DB01HP, L/R=GND, falling edge)
 *    ch1 = RIGHT = ambient mic (IMP34DT05,  L/R=VDD, rising edge)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define AUDIO_PDM_RATE_HZ   16000
#define AUDIO_PDM_CHANNELS  2
#define AUDIO_PDM_BIT_WIDTH 16

int audio_init(void);

/* ---------- One-shot capture (used by smoke test, blocking) ---------- */
int audio_record_to_wav(const char *path, int seconds,
			int32_t *peak_l, int32_t *peak_r,
			int32_t *mean_l, int32_t *mean_r);

/* ---------- Streaming recorder (async) ----------
 *
 *   audio_recorder_start(path)  → non-blocking; opens WAV file, writes
 *                                 placeholder header, starts PDM, spawns
 *                                 a writer thread. Returns 0 on success.
 *
 *   audio_recorder_stop()       → non-blocking; signals writer thread to
 *                                 finalize. The thread writes the proper
 *                                 WAV header and closes the file.
 *                                 Use audio_recorder_is_running() to wait.
 *
 *   The writer thread reads PDM blocks (~100 ms each) and streams to FAT
 *   file. No size cap.  Caller owns rotation policy.
 */
int      audio_recorder_start(const char *path);
int      audio_recorder_stop(void);
bool     audio_recorder_is_running(void);
uint32_t audio_recorder_bytes_written(void);

/* True if the writer thread exited because of an error (dmic_read,
 * dmic_trigger, fs_write, configure_dmic) rather than a stop request.
 * Cleared on the next audio_recorder_start. Used by the session manager
 * to detect a silently-dead writer.
 */
bool     audio_recorder_failed(void);
