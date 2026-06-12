#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Audio-test WAV transport through the AW MPI AI/AO engines -- the same
// engines DVR recording and playback use. The ALSA hw path (aplay/arecord)
// produces corrupted, static-laden samples on the G1's kernel in both
// directions; the MPI engines are clean on every target.

// Plays a 16-bit PCM WAV file out the codec DAC. Blocking; 0 on success.
int wav_test_play(const char *path);

// Records `seconds` of 48kHz stereo 16-bit audio from the currently selected
// record source into a WAV file. Blocking; 0 on success.
int wav_test_record(const char *path, int seconds);

#ifdef __cplusplus
}
#endif
