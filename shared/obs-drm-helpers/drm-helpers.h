// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <xf86drm.h>

#define OBS_DRM_VENDOR_UNKNOWN 0
#define OBS_DRM_VENDOR_AMD 0x1002
#define OBS_DRM_VENDOR_NVIDIA 0x10de
#define OBS_DRM_VENDOR_INTEL 0x8086

// Query device metadata without initializing VA-API, CUDA or an encoder.
// stat follows /dev/dri/by-path aliases as well as primary/render nodes.
static inline uint16_t obs_drm_device_vendor(const char *path)
{
	struct stat info;
	if (!path || stat(path, &info) != 0 || !S_ISCHR(info.st_mode))
		return OBS_DRM_VENDOR_UNKNOWN;

	drmDevicePtr device = NULL;
	if (drmGetDeviceFromDevId(info.st_rdev, 0, &device) != 0 || !device)
		return OBS_DRM_VENDOR_UNKNOWN;

	uint16_t vendor = OBS_DRM_VENDOR_UNKNOWN;
	if (device->bustype == DRM_BUS_PCI && device->deviceinfo.pci)
		vendor = device->deviceinfo.pci->vendor_id;
	drmFreeDevice(&device);
	return vendor;
}

static inline bool obs_drm_may_support_qsv(uint16_t vendor)
{
	// An unreadable/non-PCI identity must keep the original capability probe.
	return vendor == OBS_DRM_VENDOR_UNKNOWN || vendor == OBS_DRM_VENDOR_INTEL;
}

static inline const char *obs_drm_vaapi_driver_override(uint16_t vendor, const char *forced_driver)
{
	// Correct only the demonstrated incompatible override, per VADisplay.
	// Preserve deliberate iHD/i965/Mesa/custom choices and the environment.
	if (!forced_driver || strcmp(forced_driver, "nvidia") != 0)
		return NULL;
	if (vendor == OBS_DRM_VENDOR_AMD)
		return "radeonsi";
	if (vendor == OBS_DRM_VENDOR_INTEL)
		return "iHD";
	return NULL;
}
