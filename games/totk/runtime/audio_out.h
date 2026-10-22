/* Audio output device seam (docs/windows-port-plan.md C8). runtime/audio_mixer.c owns the ring buffer, prefill and
   underrun policy; a backend only owns the device and calls pull() from its own real-time thread for every device
   period. Backends: runtime/audio_out_coreaudio.c (macOS / iOS AudioUnit), runtime/audio_out_wasapi.c (Windows,
   shared-mode event-driven WASAPI).
   Contract (the same as the CoreAudio render callback had):
   - pull(ctx, out, frames) fills exactly `frames` frames of interleaved float32 at the rate and channel count given to
     audio_out_open (the mixer always asks for 2 channels at the renderer's sample rate, 48 kHz). It must not block.
   - pull may run on any thread, never concurrently with itself for one output, and never after audio_out_close
     returned. */
#pragma once
#include <stdint.h>

typedef void (*audio_out_pull_fn)(void *ctx, float *out, uint32_t frames);
typedef struct AudioOut AudioOut;

/* Opens the default output device and starts pulling. NULL: no device (the caller mixes without sound). */
AudioOut *audio_out_open(int rate, int channels, audio_out_pull_fn pull, void *ctx);
/* Starts a stopped output again (recreate 0), or rebuilds it on the current default device (recreate 1).
   Returns 1 when the output is playing afterwards, 0 when it is not (the caller then closes it). */
int audio_out_restart(AudioOut *out, int recreate);
/* Stops the device; pull is not running and will not run again once this returns. NULL is ignored. */
void audio_out_close(AudioOut *out);
/* "CoreAudio", "WASAPI": for log lines. */
const char *audio_out_backend(void);
#ifdef _WIN32
/* Output device choice (in-game menu): the active devices, and a rebuild of the open output after TOTK_AUDIO_DEVICE changed. */
int audio_out_list_devices(char (*names)[128], char (*ids)[256], int max);
void audio_out_switch_device(void);
#endif
