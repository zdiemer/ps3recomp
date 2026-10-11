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

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 cellAudioOutGetSoundAvailability(u32 audioOut, u32 type, u32 fs, u32 option)
{
    (void)audioOut;
    (void)option;

    /* Match the advertised LPCM modes. Encoded AC3/DTS is unsupported. */
    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM && fs == CELL_AUDIO_OUT_FS_48KHZ)
        return CELL_AUDIO_OUT_CHNUM_8;

    return 0; /* not available */
}

s32 cellAudioOutGetSoundAvailability2(u32 audioOut, u32 type, u32 fs, u32 ch, u32 option)
{
    (void)audioOut;
    (void)option;

    if (type == CELL_AUDIO_OUT_CODING_TYPE_LPCM &&
        fs == CELL_AUDIO_OUT_FS_48KHZ &&
        (ch == CELL_AUDIO_OUT_CHNUM_2 || ch == CELL_AUDIO_OUT_CHNUM_6 || ch == CELL_AUDIO_OUT_CHNUM_8))
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
    info->availableModeCount = 3;
    info->availableModes[0].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[0].channel = CELL_AUDIO_OUT_CHNUM_2;
    info->availableModes[0].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[1].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[1].channel = CELL_AUDIO_OUT_CHNUM_6;
    info->availableModes[1].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    info->availableModes[2].type    = CELL_AUDIO_OUT_CODING_TYPE_LPCM;
    info->availableModes[2].channel = CELL_AUDIO_OUT_CHNUM_8;
    info->availableModes[2].fs      = CELL_AUDIO_OUT_FS_48KHZ;
    u32 ea = (u32)((u8*)info - vm_base);
    vm_write32(ea + 8 + 4, 0x00000001); /* stereo */
    vm_write32(ea + 16 + 4, 0x00010000); /* 5.1 */
    vm_write32(ea + 24 + 4, 0x40000000); /* 7.1 */

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
    vm_write32(ea + 8, 0);                                  /* no down-mixer  */
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

/* cellAudioOutConfigure(audioOut, config, option, waitForEvent) -- set the
 * audio output mode. We accept any config and report success (request id 0);
 * the title queries availability separately. NID 0x4692AB35. */
s32 cellAudioOutConfigure(u32 audioOut, void* config, void* option, u32 waitForEvent)
{
    (void)config; (void)option; (void)waitForEvent;
    printf("[cellAvconfExt] AudioOutConfigure(audioOut=%u) -> ok\n", audioOut);
    return 0;
}
