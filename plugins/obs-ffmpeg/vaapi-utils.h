// SPDX-FileCopyrightText: 2022 tytan652 <tytan652@tytanium.xyz>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <util/base.h>
#include <libavutil/buffer.h>

#include <va/va.h>

VADisplay vaapi_open_device(int *fd, const char *device_path, const char *func_name);
void vaapi_close_device(int *fd, VADisplay dpy);
int vaapi_create_hwdevice(AVBufferRef **reference, const char *device_path);

// Re-probe on an explicit properties refresh; retain returned device-name
// strings until module unload. Negative capability results are cached too.
void vaapi_refresh_device_cache(void);
void vaapi_free_device_cache(void);

bool vaapi_device_rc_supported(VAProfile profile, VADisplay dpy, uint32_t rc, const char *device_path);
bool vaapi_device_bframe_supported(VAProfile profile, VADisplay dpy);

bool vaapi_display_h264_supported(VADisplay dpy, const char *device_path);
bool vaapi_device_h264_supported(const char *device_path);
const char *vaapi_get_h264_default_device(void);

bool vaapi_display_av1_supported(VADisplay dpy, const char *device_path);
bool vaapi_device_av1_supported(const char *device_path);
const char *vaapi_get_av1_default_device(void);

#ifdef ENABLE_HEVC
bool vaapi_display_hevc_supported(VADisplay dpy, const char *device_path);
bool vaapi_device_hevc_supported(const char *device_path);
const char *vaapi_get_hevc_default_device(void);
#endif
