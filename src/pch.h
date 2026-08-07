#pragma once

#include "RE/Starfield.h"
#include "SFSE/SFSE.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#ifdef ERROR
#undef ERROR
#endif

#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
