/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/apu_flags.h"

DEFINE_bool(mute, false, "Mutes all audio output.", "APU")

DEFINE_string(
    nop_audio_driver, "auto",
    "Render driver behind the nop APU (--apu=nop). 'paced': discard the "
    "samples but release the client semaphore at 48 kHz cadence, so the "
    "guest's render callback runs and titles that clock gameplay off the "
    "audio stream advance (RB3 song time). 'dummy': no driver; "
    "XAudioRegisterRenderDriverClient hands the guest a dummy handle and the "
    "render callback never runs (the behaviour before the paced driver). "
    "'auto': paced, unless a title opts out at launch -- the DC3 original "
    "XEX does, because its boot patches stub XMAHALAllocateContexts and the "
    "first render callback then faults in XMAHALWriteAndUnlockContexts and "
    "wedges the main thread.",
    "APU");
