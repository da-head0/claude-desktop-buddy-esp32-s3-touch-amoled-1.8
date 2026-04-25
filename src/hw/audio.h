#pragma once
#include <stddef.h>
#include <stdint.h>

bool hwAudioInit();
void hwBeep(uint16_t freqHz, uint16_t durMs);

// Mic capture (16 kHz mono, 16-bit PCM). Push-to-talk pattern:
// Start → Read repeatedly → Stop. Start drains any stale DMA samples
// that accumulated between captures. Concurrent beep playback shares
// the same I2S channel; emit beeps only outside an active capture
// window or the playback samples will bias the mic readback.
bool hwAudioMicStart();
bool hwAudioMicStop();
// Reads up to nSamples int16 samples from the RX DMA. Returns the
// number of samples actually read, 0 on timeout, or negative on error.
int  hwAudioMicRead(int16_t* buf, size_t nSamples, uint32_t timeoutMs);
