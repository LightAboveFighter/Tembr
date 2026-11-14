/* The single translation unit that compiles miniaudio's implementation. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

/* Drops the audio a stopped playback device still holds. Stopping only pauses
   the backend: PulseAudio corks the stream and keeps what it had buffered (a
   fraction of a second), which would then play first on the next start: the
   end of the previous track before the next one. Call it only while stopped. */
ma_result tembr_device_discard_playback(ma_device *pDevice)
{
    if (pDevice == NULL || ma_device_is_started(pDevice))
        return MA_INVALID_OPERATION;
    pDevice->playback.intermediaryBufferLen = 0;
#ifdef MA_HAS_PULSEAUDIO
    if (pDevice->pContext->backend == ma_backend_pulseaudio && pDevice->pulse.pStreamPlayback != NULL) {
        ma_pa_operation *op = ((ma_pa_stream_flush_proc)pDevice->pContext->pulse.pa_stream_flush)(
            (ma_pa_stream *)pDevice->pulse.pStreamPlayback, NULL, NULL);
        if (op != NULL)
            return ma_wait_for_operation_and_unref__pulse(pDevice->pContext, pDevice->pulse.pMainLoop, op);
    }
#endif
    /* ALSA drops its buffer on stop already; JACK keeps none. */
    return MA_SUCCESS;
}
