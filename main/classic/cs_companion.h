// main/classic/cs_companion.h -- the companion's display name.
//
// The agent on the other end of the MQTT link (calls, messages, Mines, Bingo,
// astro dice) is shown under one name: CONFIG_CS_COMPANION_NAME from
// menuconfig ("Classic shell" -> "Companion name"), "Claude" by default. Host
// builds without sdkconfig.h (tests, tools/classic/host_render.c) use the
// default. The name is drawn at runtime, never baked into the background art.
#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#endif

#ifdef CONFIG_CS_COMPANION_NAME
#define CS_COMPANION_NAME CONFIG_CS_COMPANION_NAME
#else
#define CS_COMPANION_NAME "Claude"
#endif
