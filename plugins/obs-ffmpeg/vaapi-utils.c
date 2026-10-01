// SPDX-FileCopyrightText: 2022 tytan652 <tytan652@tytanium.xyz>
//
// SPDX-License-Identifier: GPL-2.0-or-later

#include "vaapi-utils.h"

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <drm-helpers.h>

#include <va/va_drm.h>
#include <va/va_str.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>

static bool version_logged = false;

inline static VADisplay vaapi_open_display_drm(int *fd, const char *device_path)
{
	VADisplay va_dpy;

	if (!device_path)
		return NULL;

	*fd = open(device_path, O_RDWR);
	if (*fd < 0) {
		blog(LOG_ERROR, "VAAPI: Failed to open device '%s'", device_path);
		return NULL;
	}

	va_dpy = vaGetDisplayDRM(*fd);

	if (!va_dpy) {
		blog(LOG_ERROR, "VAAPI: Failed to initialize DRM display");
		close(*fd);
		*fd = -1;
		return NULL;
	}

	return va_dpy;
}

inline static void vaapi_close_display_drm(int *fd)
{
	if (*fd < 0)
		return;

	close(*fd);
	*fd = -1;
}

static void vaapi_log_info_cb(void *user_context, const char *message)
{
	UNUSED_PARAMETER(user_context);

	// Libva message always ends with a newline
	struct dstr m;
	dstr_init_copy(&m, message);
	dstr_depad(&m);

	blog(LOG_DEBUG, "Libva: %s", m.array);

	dstr_free(&m);
}

static void vaapi_log_error_cb(void *user_context, const char *message)
{
	UNUSED_PARAMETER(user_context);

	// Libva message always ends with a newline
	struct dstr m;
	dstr_init_copy(&m, message);
	dstr_depad(&m);

	blog(LOG_DEBUG, "Libva error: %s", m.array);

	dstr_free(&m);
}

VADisplay vaapi_open_device(int *fd, const char *device_path, const char *func_name)
{
	VADisplay va_dpy;
	VAStatus va_status;
	int major, minor;
	const char *driver;

	va_dpy = vaapi_open_display_drm(fd, device_path);
	if (!va_dpy)
		return NULL;

	blog(LOG_DEBUG, "VAAPI: Initializing display in %s", func_name);

	vaSetInfoCallback(va_dpy, vaapi_log_info_cb, NULL);
	vaSetErrorCallback(va_dpy, vaapi_log_error_cb, NULL);

	const char *override =
		obs_drm_vaapi_driver_override(obs_drm_device_vendor(device_path), getenv("LIBVA_DRIVER_NAME"));
	if (override) {
		va_status = vaSetDriverName(va_dpy, (char *) override);
		if (va_status != VA_STATUS_SUCCESS) {
			blog(LOG_ERROR, "VAAPI: Could not select device-specific driver %s for %s", override,
			     device_path);
			vaapi_close_device(fd, va_dpy);
			return NULL;
		}
		blog(LOG_DEBUG,
		     "VAAPI: Using device-specific driver %s for %s instead of incompatible LIBVA_DRIVER_NAME=nvidia",
		     override, device_path);
	}

	va_status = vaInitialize(va_dpy, &major, &minor);

	if (va_status != VA_STATUS_SUCCESS) {
		blog(LOG_ERROR, "VAAPI: Failed to initialize display in %s", func_name);
		vaapi_close_device(fd, va_dpy);
		return NULL;
	}

	blog(LOG_DEBUG, "VAAPI: Display initialized");

	if (!version_logged) {
		blog(LOG_INFO, "VAAPI: API version %d.%d", major, minor);
		version_logged = true;
	}

	driver = vaQueryVendorString(va_dpy);

	blog(LOG_DEBUG, "VAAPI: '%s' in use for device '%s'", driver, device_path);

	return va_dpy;
}

void vaapi_close_device(int *fd, VADisplay dpy)
{
	if (dpy)
		vaTerminate(dpy);
	vaapi_close_display_drm(fd);
}

int vaapi_create_hwdevice(AVBufferRef **reference, const char *device_path)
{
	// FFmpeg creates its own display. Apply the same per-device choice used
	// during discovery so a detected encoder can actually initialize too.
	const char *override =
		obs_drm_vaapi_driver_override(obs_drm_device_vendor(device_path), getenv("LIBVA_DRIVER_NAME"));
	AVDictionary *options = NULL;
	int result = override ? av_dict_set(&options, "driver", override, 0) : 0;
	if (result >= 0)
		result = av_hwdevice_ctx_create(reference, AV_HWDEVICE_TYPE_VAAPI, device_path, options, 0);
	av_dict_free(&options);
	return result;
}

enum vaapi_codec_capability {
	VAAPI_CAP_H264 = 1 << 0,
	VAAPI_CAP_AV1 = 1 << 1,
	VAAPI_CAP_HEVC = 1 << 2,
};

static unsigned int vaapi_cached_capabilities(const char *device_path);
static const char *vaapi_default_device(enum vaapi_codec_capability codec);

static uint32_t vaapi_display_ep_combo_rate_controls(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy,
						     const char *device_path)
{
	VAStatus va_status;
	VAConfigAttrib attrib[1];
	attrib->type = VAConfigAttribRateControl;

	va_status = vaGetConfigAttributes(dpy, profile, entrypoint, attrib, 1);

	switch (va_status) {
	case VA_STATUS_SUCCESS:
		return attrib->value;
	case VA_STATUS_ERROR_UNSUPPORTED_PROFILE:
		blog(LOG_DEBUG, "VAAPI: %s is not supported by the device '%s'", vaProfileStr(profile), device_path);
		return 0;
	case VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT:
		blog(LOG_DEBUG, "VAAPI: %s %s is not supported by the device '%s'", vaProfileStr(profile),
		     vaEntrypointStr(entrypoint), device_path);
		return 0;
	default:
		blog(LOG_ERROR, "VAAPI: Fail to get RC attribute from the %s %s of the device '%s'",
		     vaProfileStr(profile), vaEntrypointStr(entrypoint), device_path);
		return 0;
	}
}

static bool vaapi_display_ep_combo_supported(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy,
					     const char *device_path)
{
	uint32_t ret = vaapi_display_ep_combo_rate_controls(profile, entrypoint, dpy, device_path);
	if (ret & VA_RC_CBR || ret & VA_RC_CQP || ret & VA_RC_VBR)
		return true;

	return false;
}

bool vaapi_device_rc_supported(VAProfile profile, VADisplay dpy, uint32_t rc, const char *device_path)
{
	uint32_t ret = vaapi_display_ep_combo_rate_controls(profile, VAEntrypointEncSlice, dpy, device_path);
	if (ret & rc)
		return true;
	ret = vaapi_display_ep_combo_rate_controls(profile, VAEntrypointEncSliceLP, dpy, device_path);
	if (ret & rc)
		return true;

	return false;
}

static bool vaapi_display_ep_bframe_supported(VAProfile profile, VAEntrypoint entrypoint, VADisplay dpy)
{
	VAStatus va_status;
	VAConfigAttrib attrib[1];
	attrib->type = VAConfigAttribEncMaxRefFrames;

	va_status = vaGetConfigAttributes(dpy, profile, entrypoint, attrib, 1);

	if (va_status == VA_STATUS_SUCCESS && attrib->value != VA_ATTRIB_NOT_SUPPORTED)
		return attrib->value >> 16;

	return false;
}

bool vaapi_device_bframe_supported(VAProfile profile, VADisplay dpy)
{
	bool ret = vaapi_display_ep_bframe_supported(profile, VAEntrypointEncSlice, dpy);
	if (ret)
		return true;
	ret = vaapi_display_ep_bframe_supported(profile, VAEntrypointEncSliceLP, dpy);
	if (ret)
		return true;

	return false;
}

#define CHECK_PROFILE(ret, profile, va_dpy, device_path)                                              \
	if (vaapi_display_ep_combo_supported(profile, VAEntrypointEncSlice, va_dpy, device_path)) {   \
		blog(LOG_DEBUG, "'%s' support encoding with %s", device_path, vaProfileStr(profile)); \
		ret |= true;                                                                          \
	}

#define CHECK_PROFILE_LP(ret, profile, va_dpy, device_path)                                                     \
	if (vaapi_display_ep_combo_supported(profile, VAEntrypointEncSliceLP, va_dpy, device_path)) {           \
		blog(LOG_DEBUG, "'%s' support low power encoding with %s", device_path, vaProfileStr(profile)); \
		ret |= true;                                                                                    \
	}

bool vaapi_display_h264_supported(VADisplay dpy, const char *device_path)
{
	bool ret = false;

	CHECK_PROFILE(ret, VAProfileH264ConstrainedBaseline, dpy, device_path);
	CHECK_PROFILE(ret, VAProfileH264Main, dpy, device_path);
	CHECK_PROFILE(ret, VAProfileH264High, dpy, device_path);

	if (!ret) {
		CHECK_PROFILE_LP(ret, VAProfileH264ConstrainedBaseline, dpy, device_path);
		CHECK_PROFILE_LP(ret, VAProfileH264Main, dpy, device_path);
		CHECK_PROFILE_LP(ret, VAProfileH264High, dpy, device_path);
	}

	return ret;
}

bool vaapi_device_h264_supported(const char *device_path)
{
	return (vaapi_cached_capabilities(device_path) & VAAPI_CAP_H264) != 0;
}

const char *vaapi_get_h264_default_device(void)
{
	return vaapi_default_device(VAAPI_CAP_H264);
}

bool vaapi_display_av1_supported(VADisplay dpy, const char *device_path)
{
	bool ret = false;

	CHECK_PROFILE(ret, VAProfileAV1Profile0, dpy, device_path);

	if (!ret) {
		CHECK_PROFILE_LP(ret, VAProfileAV1Profile0, dpy, device_path);
	}

	return ret;
}

bool vaapi_device_av1_supported(const char *device_path)
{
	return (vaapi_cached_capabilities(device_path) & VAAPI_CAP_AV1) != 0;
}

const char *vaapi_get_av1_default_device(void)
{
	return vaapi_default_device(VAAPI_CAP_AV1);
}

#ifdef ENABLE_HEVC

bool vaapi_display_hevc_supported(VADisplay dpy, const char *device_path)
{
	bool ret = false;

	CHECK_PROFILE(ret, VAProfileHEVCMain, dpy, device_path);
	CHECK_PROFILE(ret, VAProfileHEVCMain10, dpy, device_path);

	if (!ret) {
		CHECK_PROFILE_LP(ret, VAProfileHEVCMain, dpy, device_path);
		CHECK_PROFILE_LP(ret, VAProfileHEVCMain10, dpy, device_path);
	}

	return ret;
}

bool vaapi_device_hevc_supported(const char *device_path)
{
	return (vaapi_cached_capabilities(device_path) & VAAPI_CAP_HEVC) != 0;
}

const char *vaapi_get_hevc_default_device(void)
{
	return vaapi_default_device(VAAPI_CAP_HEVC);
}

#endif // #ifdef ENABLE_HEVC

struct vaapi_device_capabilities {
	char *path;
	struct stat identity;
	bool valid;
	unsigned int codecs;
	uint64_t retry_after;
};

static DARRAY(struct vaapi_device_capabilities) device_cache;
static pthread_mutex_t device_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Caller holds device_cache_mutex. All three codecs share one VA display,
// including on decode-only devices where every result is negative.
static struct vaapi_device_capabilities *vaapi_query_device(const char *path)
{
	struct stat identity;
	if (!path || stat(path, &identity) != 0 || !S_ISCHR(identity.st_mode))
		return NULL;

	char canonical[PATH_MAX];
	if (realpath(path, canonical))
		path = canonical;
	struct vaapi_device_capabilities *entry = NULL;
	for (size_t i = 0; i < device_cache.num; i++) {
		if (strcmp(device_cache.array[i].path, path) == 0) {
			entry = &device_cache.array[i];
			break;
		}
	}
	if (!entry) {
		struct vaapi_device_capabilities added = {.path = bstrdup(path)};
		size_t index = da_push_back(device_cache, &added);
		entry = &device_cache.array[index];
	}

	const uint64_t now = os_gettime_ns();
	if (entry->valid && entry->identity.st_dev == identity.st_dev && entry->identity.st_ino == identity.st_ino &&
	    entry->identity.st_rdev == identity.st_rdev && entry->identity.st_ctime == identity.st_ctime &&
	    (!entry->retry_after || now < entry->retry_after))
		return entry;

	entry->identity = identity;
	entry->codecs = 0;
	entry->valid = true;
	// Suppress immediate repeat attempts, but do not cache transient init
	// failures (permissions/device busy/driver reset) for the entire session.
	entry->retry_after = now + 1000000000ULL;
	int fd = -1;
	VADisplay display = vaapi_open_device(&fd, path, "capability discovery");
	if (!display)
		return entry;
	if (vaapi_display_h264_supported(display, path))
		entry->codecs |= VAAPI_CAP_H264;
	if (vaapi_display_av1_supported(display, path))
		entry->codecs |= VAAPI_CAP_AV1;
#ifdef ENABLE_HEVC
	if (vaapi_display_hevc_supported(display, path))
		entry->codecs |= VAAPI_CAP_HEVC;
#endif
	vaapi_close_device(&fd, display);
	entry->retry_after = 0;
	return entry;
}

static unsigned int vaapi_cached_capabilities(const char *device_path)
{
	pthread_mutex_lock(&device_cache_mutex);
	struct vaapi_device_capabilities *entry = vaapi_query_device(device_path);
	unsigned int codecs = entry ? entry->codecs : 0;
	pthread_mutex_unlock(&device_cache_mutex);
	return codecs;
}

static const char *vaapi_default_device(enum vaapi_codec_capability codec)
{
	// Enumerate actual nodes, not a contiguous sequence that stops at the
	// first missing number after a hot-unplug. glob sorts the device paths.
	glob_t nodes = {0};
	const char *result = NULL;
	if (glob("/dev/dri/renderD*", 0, NULL, &nodes) == 0) {
		pthread_mutex_lock(&device_cache_mutex);
		for (size_t i = 0; i < nodes.gl_pathc; i++) {
			struct vaapi_device_capabilities *entry = vaapi_query_device(nodes.gl_pathv[i]);
			if (entry && (entry->codecs & codec)) {
				result = entry->path;
				break;
			}
		}
		pthread_mutex_unlock(&device_cache_mutex);
	}
	globfree(&nodes);
	return result;
}

void vaapi_refresh_device_cache(void)
{
	pthread_mutex_lock(&device_cache_mutex);
	for (size_t i = 0; i < device_cache.num; i++)
		device_cache.array[i].valid = false;
	pthread_mutex_unlock(&device_cache_mutex);
}

void vaapi_free_device_cache(void)
{
	pthread_mutex_lock(&device_cache_mutex);
	for (size_t i = 0; i < device_cache.num; i++)
		bfree(device_cache.array[i].path);
	da_free(device_cache);
	pthread_mutex_unlock(&device_cache_mutex);
}
