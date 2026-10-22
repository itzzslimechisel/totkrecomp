/* audio_out.h on Windows: shared-mode, event-driven WASAPI on the default render device (C8).
   One thread per output owns every COM object (MTA): it opens the device, asks for the mixer's own stream (float32,
   `channels`, `rate`) with AUTOCONVERTPCM so the audio engine converts rate and channel layout itself, and falls back to
   the device mix format (runtime/audio_convert.h) when that is refused and the rates agree. Each buffer event it asks
   pull() for exactly the free part of the device buffer, so pull() sees the same "fill N frames now" contract as the
   CoreAudio render callback. The device is rebuilt by the thread itself when it disappears (AUDCLNT_E_DEVICE_INVALIDATED:
   unplugged headphones, a driver reset) and when the default output device changes (IMMNotificationClient), which is
   what runtime_audio_output_restart does by hand on iOS. Thread priority: MMCSS "Pro Audio" (avrt). */
#if defined(_WIN32)
#define COBJMACROS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <initguid.h>   /* after the SDK headers: only the GUIDs below are defined here */
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "audio_out.h"
#include "audio_convert.h"

/* Own GUID definitions: not every MinGW import library carries them. */
DEFINE_GUID(TOTK_CLSID_MMDeviceEnumerator, 0xbcde0395, 0xe52f, 0x467c, 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e);
DEFINE_GUID(TOTK_IID_IMMDeviceEnumerator, 0xa95664d2, 0x9614, 0x4f35, 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6);
DEFINE_GUID(TOTK_IID_IMMNotificationClient, 0x7991eec9, 0x7e89, 0x4d85, 0x83, 0x90, 0x6c, 0x70, 0x3c, 0xec, 0x60, 0xc0);
DEFINE_GUID(TOTK_IID_IAudioClient, 0x1cb9ad4c, 0xdbfa, 0x4c32, 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2);
DEFINE_GUID(TOTK_IID_IAudioRenderClient, 0xf294acfc, 0x3146, 0x4483, 0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2);
DEFINE_GUID(TOTK_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 0x00000003, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(TOTK_KSDATAFORMAT_SUBTYPE_PCM, 0x00000001, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(TOTK_IID_IUnknown, 0x00000000, 0x0000, 0x0000, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46);

#ifndef AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
#define AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM 0x80000000
#endif
#ifndef AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
#define AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 0x08000000
#endif

enum { STATE_STARTING, STATE_RUNNING, STATE_FAILED };

struct AudioOut {
    int rate, channels;
    audio_out_pull_fn pull; void *ctx;
    HANDLE thread, stop, rebuild;       /* rebuild: signalled by the notification client (default device changed) */
    _Atomic int state;                  /* STATE_*: the thread's first open */
    HANDLE ready;                       /* set once state leaves STATE_STARTING */
    /* the audio thread's own */
    IMMDeviceEnumerator *enumerator;
    IMMNotificationClient notify; LONG notify_refs; int notify_registered;
    IAudioClient *client; IAudioRenderClient *render; HANDLE event;
    UINT32 buffer_frames; int dev_channels; AudioSampleFormat dev_fmt; int native;   /* native: the device takes float32 `channels` as is */
    float *scratch; UINT32 scratch_frames;
    uint64_t rebuilds;
};

const char *audio_out_backend(void) { return "WASAPI"; }
static AudioOut *g_current;   /* the open output (one per process) */

/* ---- default-device notifications (IMMNotificationClient in C) ---- */
#define NOTIFY_OWNER(p) ((AudioOut *)((char *)(p) - offsetof(AudioOut, notify)))
static HRESULT STDMETHODCALLTYPE n_query(IMMNotificationClient *self, REFIID riid, void **out) {
    if (IsEqualIID(riid, &TOTK_IID_IUnknown) || IsEqualIID(riid, &TOTK_IID_IMMNotificationClient)) {
        *out = self; InterlockedIncrement(&NOTIFY_OWNER(self)->notify_refs); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE n_addref(IMMNotificationClient *self) { return (ULONG)InterlockedIncrement(&NOTIFY_OWNER(self)->notify_refs); }
static ULONG STDMETHODCALLTYPE n_release(IMMNotificationClient *self) { return (ULONG)InterlockedDecrement(&NOTIFY_OWNER(self)->notify_refs); }   /* embedded: freed with the AudioOut */
static HRESULT STDMETHODCALLTYPE n_state(IMMNotificationClient *self, LPCWSTR id, DWORD st) { (void)self; (void)id; (void)st; return S_OK; }
static HRESULT STDMETHODCALLTYPE n_added(IMMNotificationClient *self, LPCWSTR id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE n_removed(IMMNotificationClient *self, LPCWSTR id) { (void)self; (void)id; return S_OK; }
static HRESULT STDMETHODCALLTYPE n_default(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR id) {
    (void)id;
    if (flow == eRender && role == eConsole) SetEvent(NOTIFY_OWNER(self)->rebuild);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE n_prop(IMMNotificationClient *self, LPCWSTR id, const PROPERTYKEY key) { (void)self; (void)id; (void)key; return S_OK; }
static IMMNotificationClientVtbl notify_vtbl = {n_query, n_addref, n_release, n_state, n_added, n_removed, n_default, n_prop};

/* ---- device ---- */
/* The output endpoint: TOTK_AUDIO_DEVICE (an endpoint id from audio_out_list_devices, set by the in-game menu) when that
   device is present and active, else the system default. */
static HRESULT get_endpoint(AudioOut *o, IMMDevice **dev) {
    const char *want = getenv("TOTK_AUDIO_DEVICE");
    if (want && *want) {
        wchar_t id[512];
        if (MultiByteToWideChar(CP_UTF8, 0, want, -1, id, 512) && SUCCEEDED(IMMDeviceEnumerator_GetDevice(o->enumerator, id, dev))) {
            DWORD st = 0;
            if (SUCCEEDED(IMMDevice_GetState(*dev, &st)) && st == DEVICE_STATE_ACTIVE) return S_OK;
            IMMDevice_Release(*dev); *dev = NULL;
        }
    }
    return IMMDeviceEnumerator_GetDefaultAudioEndpoint(o->enumerator, eRender, eConsole, dev);
}
static void device_close(AudioOut *o) {
    if (o->client) IAudioClient_Stop(o->client);
    if (o->render) { IAudioRenderClient_Release(o->render); o->render = NULL; }
    if (o->client) { IAudioClient_Release(o->client); o->client = NULL; }
}

static int describe_mix(AudioOut *o, const WAVEFORMATEX *w) {
    int is_float = w->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, valid = w->wBitsPerSample;
    if (w->wFormatTag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *x = (const WAVEFORMATEXTENSIBLE *)w;
        if (IsEqualGUID(&x->SubFormat, &TOTK_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) is_float = 1;
        else if (!IsEqualGUID(&x->SubFormat, &TOTK_KSDATAFORMAT_SUBTYPE_PCM)) return 0;
        valid = x->Samples.wValidBitsPerSample;
    } else if (w->wFormatTag != WAVE_FORMAT_PCM && !is_float) return 0;
    int f = audio_fmt_from_wave(is_float, w->wBitsPerSample, valid);
    if (f < 0 || w->nChannels < 1) return 0;
    o->dev_fmt = (AudioSampleFormat)f; o->dev_channels = w->nChannels;
    return 1;
}

static int device_open(AudioOut *o, int quiet) {
    IMMDevice *dev = NULL; WAVEFORMATEX *mix = NULL; HRESULT hr;
    hr = get_endpoint(o, &dev);
    if (FAILED(hr)) { if (!quiet) fprintf(stderr, "[totk-audio] 【红】no default output device (0x%08lx): mixing without sound\n", (unsigned long)hr); return 0; }
    hr = IMMDevice_Activate(dev, &TOTK_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&o->client);
    IMMDevice_Release(dev);
    if (FAILED(hr)) goto fail;
    /* 1. The mixer's own stream; the engine converts rate and layout. */
    WAVEFORMATEXTENSIBLE want = {0};
    want.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; want.Format.nChannels = (WORD)o->channels; want.Format.nSamplesPerSec = (DWORD)o->rate;
    want.Format.wBitsPerSample = 32; want.Format.nBlockAlign = (WORD)(4 * o->channels); want.Format.nAvgBytesPerSec = (DWORD)o->rate * want.Format.nBlockAlign;
    want.Format.cbSize = 22; want.Samples.wValidBitsPerSample = 32;
    want.dwChannelMask = o->channels == 1 ? SPEAKER_FRONT_CENTER : o->channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    want.SubFormat = TOTK_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = IAudioClient_Initialize(o->client, AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                 0, 0, (WAVEFORMATEX *)&want, NULL);
    if (SUCCEEDED(hr)) { o->native = 1; o->dev_fmt = AUDIO_FMT_F32; o->dev_channels = o->channels; }
    else {
        /* 2. The device mix format, converted here (same rate only: no resampler of our own). */
        IAudioClient_Release(o->client); o->client = NULL;
        hr = get_endpoint(o, &dev);
        if (FAILED(hr)) goto fail;
        hr = IMMDevice_Activate(dev, &TOTK_IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&o->client);
        IMMDevice_Release(dev);
        if (FAILED(hr)) goto fail;
        hr = IAudioClient_GetMixFormat(o->client, &mix);
        if (FAILED(hr)) goto fail;
        if ((int)mix->nSamplesPerSec != o->rate || !describe_mix(o, mix) || o->channels != 2) {
            if (!quiet) fprintf(stderr, "[totk-audio] 【红】WASAPI: the device runs %lu Hz / %u channels / %u bits and refused a converted stream: mixing without sound\n",
                                (unsigned long)mix->nSamplesPerSec, mix->nChannels, mix->wBitsPerSample);
            CoTaskMemFree(mix); device_close(o); return 0;
        }
        hr = IAudioClient_Initialize(o->client, AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, mix, NULL);
        CoTaskMemFree(mix); mix = NULL;
        if (FAILED(hr)) goto fail;
        o->native = 0;
    }
    if (FAILED(hr = IAudioClient_SetEventHandle(o->client, o->event))) goto fail;
    if (FAILED(hr = IAudioClient_GetBufferSize(o->client, &o->buffer_frames))) goto fail;
    if (FAILED(hr = IAudioClient_GetService(o->client, &TOTK_IID_IAudioRenderClient, (void **)&o->render))) goto fail;
    if (o->buffer_frames > o->scratch_frames) {
        free(o->scratch); o->scratch = malloc((size_t)o->buffer_frames * 2 * sizeof(float));
        if (!o->scratch) { o->scratch_frames = 0; hr = E_OUTOFMEMORY; goto fail; }
        o->scratch_frames = o->buffer_frames;
    }
    /* Start on a silent buffer: the first event then asks for one period of real audio. */
    BYTE *p;
    if (SUCCEEDED(IAudioRenderClient_GetBuffer(o->render, o->buffer_frames, &p))) IAudioRenderClient_ReleaseBuffer(o->render, o->buffer_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(hr = IAudioClient_Start(o->client))) goto fail;
    REFERENCE_TIME period = 0; IAudioClient_GetDevicePeriod(o->client, &period, NULL);
    fprintf(stderr, "[totk-audio] WASAPI output %d Hz %s (%s), buffer %u frames, period %.1f ms\n", o->rate, o->channels == 2 ? "stereo" : "multichannel",
            o->native ? "float32, engine-converted" : "device mix format", (unsigned)o->buffer_frames, period / 10000.0);
    return 1;
fail:
    if (mix) CoTaskMemFree(mix);
    if (!quiet) fprintf(stderr, "[totk-audio] 【红】WASAPI output setup failed (0x%08lx): mixing without sound\n", (unsigned long)hr);
    device_close(o);
    return 0;
}

/* Fills what the device buffer has free. 0: the device is gone (rebuild). */
static int feed(AudioOut *o) {
    UINT32 padding = 0; BYTE *p = NULL;
    HRESULT hr = IAudioClient_GetCurrentPadding(o->client, &padding);
    if (FAILED(hr)) return 0;
    UINT32 n = o->buffer_frames - padding;
    if (!n) return 1;
    hr = IAudioRenderClient_GetBuffer(o->render, n, &p);
    if (FAILED(hr)) return 0;
    if (o->native) o->pull(o->ctx, (float *)p, n);
    else { o->pull(o->ctx, o->scratch, n); audio_convert_stereo(o->scratch, n, p, o->dev_fmt, o->dev_channels); }
    hr = IAudioRenderClient_ReleaseBuffer(o->render, n, 0);
    return SUCCEEDED(hr);
}

static void set_state(AudioOut *o, int s) { atomic_store(&o->state, s); SetEvent(o->ready); }

static DWORD WINAPI audio_thread(void *arg) {
    AudioOut *o = arg;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task = 0; HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (FAILED(CoCreateInstance(&TOTK_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &TOTK_IID_IMMDeviceEnumerator, (void **)&o->enumerator))) {
        fprintf(stderr, "[totk-audio] 【红】WASAPI: no device enumerator: mixing without sound\n");
        set_state(o, STATE_FAILED); goto out;
    }
    o->notify.lpVtbl = &notify_vtbl; o->notify_refs = 1;
    o->notify_registered = SUCCEEDED(IMMDeviceEnumerator_RegisterEndpointNotificationCallback(o->enumerator, &o->notify));
    if (!device_open(o, 0)) { set_state(o, STATE_FAILED); goto out; }
    set_state(o, STATE_RUNNING);
    HANDLE waits[3] = {o->stop, o->rebuild, o->event};
    for (;;) {
        DWORD w = WaitForMultipleObjects(3, waits, FALSE, o->client ? 200 : 500);
        if (w == WAIT_OBJECT_0) break;
        int rebuild = w == WAIT_OBJECT_0 + 1;
        if (!rebuild && o->client && (w == WAIT_OBJECT_0 + 2 || w == WAIT_TIMEOUT)) rebuild = !feed(o);
        if (!rebuild && o->client) continue;
        /* Device gone or default changed: rebuild on the current default device, retrying while there is none. */
        int had = o->client != NULL;
        device_close(o);
        if (device_open(o, !had)) fprintf(stderr, "[totk-audio] WASAPI output rebuilt on the %s device (%llu)\n", getenv("TOTK_AUDIO_DEVICE") && *getenv("TOTK_AUDIO_DEVICE") ? "chosen" : "default", (unsigned long long)++o->rebuilds);
        else if (had) fprintf(stderr, "[totk-audio] WASAPI output lost: retrying every 0.5 s\n");
    }
out:
    device_close(o);
    if (o->enumerator) {
        if (o->notify_registered) IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(o->enumerator, &o->notify);
        IMMDeviceEnumerator_Release(o->enumerator); o->enumerator = NULL;
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

static int thread_start(AudioOut *o) {
    atomic_store(&o->state, STATE_STARTING);
    ResetEvent(o->stop); ResetEvent(o->rebuild); ResetEvent(o->ready);
    o->thread = CreateThread(NULL, 0, audio_thread, o, 0, NULL);
    if (!o->thread) return 0;
    WaitForSingleObject(o->ready, INFINITE);
    if (atomic_load(&o->state) != STATE_RUNNING) { WaitForSingleObject(o->thread, INFINITE); CloseHandle(o->thread); o->thread = NULL; return 0; }
    return 1;
}
static void thread_stop(AudioOut *o) {
    if (!o->thread) return;
    SetEvent(o->stop); WaitForSingleObject(o->thread, INFINITE); CloseHandle(o->thread); o->thread = NULL;
}

static void out_free(AudioOut *o) {
    if (o->stop) CloseHandle(o->stop);
    if (o->rebuild) CloseHandle(o->rebuild);
    if (o->ready) CloseHandle(o->ready);
    if (o->event) CloseHandle(o->event);
    free(o->scratch); free(o);
}

AudioOut *audio_out_open(int rate, int channels, audio_out_pull_fn pull, void *ctx) {
    if (rate <= 0 || channels < 1 || channels > 8 || !pull) return NULL;
    AudioOut *o = calloc(1, sizeof *o);
    if (!o) return NULL;
    o->rate = rate; o->channels = channels; o->pull = pull; o->ctx = ctx;
    o->stop = CreateEventW(NULL, TRUE, FALSE, NULL); o->rebuild = CreateEventW(NULL, FALSE, FALSE, NULL);
    o->ready = CreateEventW(NULL, TRUE, FALSE, NULL); o->event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!o->stop || !o->rebuild || !o->ready || !o->event || !thread_start(o)) { out_free(o); return NULL; }
    g_current = o;
    return o;
}
int audio_out_restart(AudioOut *o, int recreate) {
    if (!o) return 0;
    if (!recreate && o->thread && WaitForSingleObject(o->thread, 0) == WAIT_TIMEOUT) return 1;   /* still running (it rebuilds itself) */
    thread_stop(o);
    return thread_start(o);
}
void audio_out_close(AudioOut *o) {
    if (!o) return;
    if (g_current == o) g_current = NULL;
    thread_stop(o); out_free(o);
}

/* The in-game menu (runtime/overlay): after TOTK_AUDIO_DEVICE changed, rebuild the open output on that device. */
void audio_out_switch_device(void) { AudioOut *o = g_current; if (o && o->rebuild) SetEvent(o->rebuild); }

/* Active output devices: names (friendly names, UTF-8) and ids (for TOTK_AUDIO_DEVICE). Any thread. */
static const PROPERTYKEY TOTK_PKEY_Device_FriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
int audio_out_list_devices(char (*names)[128], char (*ids)[256], int max) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDeviceCollection *col = NULL; UINT n = 0; int out = 0;
    if (SUCCEEDED(CoCreateInstance(&TOTK_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &TOTK_IID_IMMDeviceEnumerator, (void **)&en)) &&
        SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col)) && SUCCEEDED(IMMDeviceCollection_GetCount(col, &n))) {
        for (UINT i = 0; i < n && out < max; i++) {
            IMMDevice *d = NULL; LPWSTR id = NULL; IPropertyStore *ps = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            if (SUCCEEDED(IMMDevice_GetId(d, &id))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                names[out][0] = 0;
                if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps)) && SUCCEEDED(IPropertyStore_GetValue(ps, &TOTK_PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, names[out], 128, NULL, NULL);
                PropVariantClear(&pv);
                if (ps) IPropertyStore_Release(ps);
                if (!names[out][0]) snprintf(names[out], 128, "Device %u", i + 1);
                WideCharToMultiByte(CP_UTF8, 0, id, -1, ids[out], 256, NULL, NULL);
                CoTaskMemFree(id); out++;
            }
            IMMDevice_Release(d);
        }
    }
    if (col) IMMDeviceCollection_Release(col);
    if (en) IMMDeviceEnumerator_Release(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return out;
}
#endif /* _WIN32 */
