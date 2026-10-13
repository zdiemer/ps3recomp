/*
 * ps3recomp - cellAvconfExt HLE implementation
 *
 * Reports standard LPCM stereo 48kHz as available audio output.
 * Games query this to decide audio format before opening cellAudio ports.
 */

#include "cellAvconfExt.h"
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write32: translate + byte-swap */

/* Output pointers are guest addresses. Sound modes occupy eight bytes,
 * starting at device offset 8; multi-byte fields are big-endian. */
extern u8* vm_base;
#define GUEST_PTR(p, T) ((T)((p) ? (void*)(vm_base + (uint32_t)(uintptr_t)(p)) : (void*)0))
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static float s_gamma = 1.0f;
static u32 s_downmixer;
u32 ps3_audio_out_downmixer(void)
{
    return __atomic_load_n(&s_downmixer, __ATOMIC_RELAXED);
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    /* All current host backends submit stereo PCM. Advertising surround here
     * makes titles configure an eight-channel output without a downmixer. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM && fs == CELL_AUDIO_OUT_FS_48KHZ)
        return CELL_AUDIO_OUT_CHNUM_2;

    return 0; /* not available */
}

s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option)
{
    (void)audioOut;
    (void)option;

    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
        fs == CELL_AUDIO_OUT_FS_48KHZ &&
        ch == CELL_AUDIO_OUT_CHNUM_2)
        return 1; /* available */

    return 0;
}

s32 cellAudioOutGetDeviceInfo(u32 audioOut, u32 deviceIndex,
                               CellAudioOutDeviceInfo* info)
{
    (void)audioOut;
    (void)deviceIndex;

    printf("[cellAvconfExt] AudioOutGetDeviceInfo(out=%u, dev=%u)\n",
           audioOut, deviceIndex);

    if (!info)
        return CELL_EINVAL;
    info = GUEST_PTR(info, CellAudioOutDeviceInfo*);

    memset(info, 0, sizeof(CellAudioOutDeviceInfo));

    /* Retail ABI: eight-byte header and eight-byte sound modes. */
    info->portType = 0; /* HDMI */
    info->state = 1;    /* CELL_AUDIO_OUT_DEVICE_STATE_AVAILABLE */
    info->availableModeCount = 1;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    u32 ea = (u32)((u8*)info - vm_base);
    vm_write32(ea + 8 + 4, 0x00000001); /* stereo */

    return CELL_OK;
}

s32 cellAudioOutGetConfiguration(u32 audioOut,
                                  CellAudioOutConfiguration* config,
                                  void* option, u32 optionSize)
{
    (void)audioOut;
    (void)option;
    (void)optionSize;

    printf("[cellAvconfExt] AudioOutGetConfiguration()\n");

    if (!config)
        return CELL_EINVAL;
    config = GUEST_PTR(config, CellAudioOutConfiguration*);

    memset(config, 0, sizeof(CellAudioOutConfiguration));
    config->channel = CELL_AUDIO_OUT_CHNUM_2;
    config->encoder = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    vm_write32((u32)((u8*)config - vm_base) + 12,
               audioOut == 0 ? ps3_audio_out_downmixer() : 0);

    return CELL_OK;
}

/* cellAudioOutGetState(audioOut, deviceIndex, state) -- the audio sibling of
 * cellVideoOutGetState. A title checks it before configuring output; Virtua
 * Fighter 5 calls it once during boot and it was the last unresolved import in
 * the whole title. The device is always present and enabled here: there is no
 * "no audio device" case a recompiled port can be in.
 *
 * CellAudioOutState { u8 state; u8 encoder; u8 reserved[6]; u32 downMixer;
 *                     CellAudioOutSoundMode soundMode; } -- written big-endian
 * a byte at a time, which needs no struct definition to get right. */
s32 cellAudioOutGetState(u32 audioOut, u32 deviceIndex, void* state)
{
    (void)deviceIndex;
    uint32_t ea = (uint32_t)(uintptr_t)state;
    printf("[cellAvconfExt] AudioOutGetState(audioOut=%u)\n", audioOut);
    if (!ea)
        return CELL_EINVAL;

    vm_write8(ea + 0, 0);                                   /* ENABLED        */
    vm_write8(ea + 1, CELL_AUDIO_OUT_CODING_TYPE_LPCM);
    vm_write8(ea + 2, 0);
    vm_write8(ea + 3, 0);
    vm_write32(ea + 4, 0);                                  /* reserved       */
    vm_write32(ea + 8, audioOut == 0 ? ps3_audio_out_downmixer() : 0);
    vm_write8(ea + 12, CELL_AUDIO_OUT_CODING_TYPE_LPCM);       /* soundMode      */
    vm_write8(ea + 13, CELL_AUDIO_OUT_CHNUM_2);
    vm_write8(ea + 14, CELL_AUDIO_OUT_FS_48KHZ);
    vm_write8(ea + 15, 0);
    vm_write32(ea + 16, 0x00000001);
    return CELL_OK;
}

s32 cellAudioOutSetCopyControl(u32 audioOut, u32 control)
{
    (void)audioOut;
    printf("[cellAvconfExt] AudioOutSetCopyControl(control=%u)\n", control);
    return CELL_OK;
}

s32 cellAudioOutGetNumberOfDevice(u32 audioOut)
{
    (void)audioOut;
    return 1; /* one audio output device */
}

s32 cellVideoOutGetGamma(u32 videoOut, float* gamma)
{
    (void)videoOut;
    if (!gamma) return CELL_EINVAL;
    { float g = s_gamma; uint32_t bits; memcpy(&bits, &g, 4);
      vm_write32((uint32_t)(uintptr_t)gamma, bits); }
    return CELL_OK;
}

s32 cellVideoOutSetGamma(u32 videoOut, float gamma)
{
    (void)videoOut;
    printf("[cellAvconfExt] VideoOutSetGamma(%.2f)\n", gamma);
    s_gamma = gamma;
    return CELL_OK;
}

/* The physical output remains stereo. Preserve a valid primary downmixer
 * selection; as on the guest system, an invalid mode keeps the previous one.
 * CellAudioOutConfiguration has ten reserved bytes and downMixer at +12. */
s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    u32 ea = (u32)(uintptr_t)config;
    if (!ea) return CELL_EINVAL;
    if (audioOut == 0 && vm_read32(ea + 12) <= 2)
        __atomic_store_n(&s_downmixer, vm_read32(ea + 12), __ATOMIC_RELAXED);
    fprintf(stderr, "[audio-config] out=%u channels=%u encoder=%u downmixer=%u\n", audioOut, vm_read8(ea), vm_read8(ea+1), vm_read32(ea+12));
    (void)option; (void)waitForEvent;
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u) -> ok\n", audioOut);
    return 0;
}
