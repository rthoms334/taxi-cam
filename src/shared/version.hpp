#pragma once

// Generated from the newest changelog.json entry by build.ps1.
#include "taxi-cam-version.hpp"

#define TAXI_CAM_WIDEN_IMPL(value) L##value
#define TAXI_CAM_WIDEN(value) TAXI_CAM_WIDEN_IMPL(value)
#define TAXI_CAM_VERSION_WIDE TAXI_CAM_WIDEN(TAXI_CAM_VERSION)
