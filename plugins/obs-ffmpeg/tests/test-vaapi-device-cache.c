// SPDX-License-Identifier: GPL-2.0-or-later

#include "../vaapi-utils.h"
#include <util/bmem.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <va/va_drm.h>
#include <va/va_str.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <xf86drm.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <pthread.h>

#define CHECK(condition)                                                           \
	do {                                                                       \
		if (!(condition)) {                                                \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
			exit(1);                                                   \
		}                                                                  \
	} while (0)

/* Run the production discovery code against a decode-only NVIDIA device and
 * an AMD encoder. There is deliberately a gap in the render-node numbers. */
static char *paths[] = {"/dev/dri/renderD128", "/dev/dri/renderD130", NULL};
static const char *alias = "/dev/dri/by-path/pci-amd-render";
static uint64_t now_ns = 1000000000ULL;
static unsigned opens[2], closes[2], initializations[2], terminations[2];
static unsigned inode_generation[2];
static char driver_overrides[2][32];
static bool fail_initialize, fail_display, fail_driver, fail_metadata, missing_device;
static bool non_pci, regular_file;
static int hwdevice_result;
static unsigned hwdevice_calls;
static const char *expected_hwdriver;

static int device_index(const char *path)
{
	if (path && strcmp(path, paths[0]) == 0)
		return 0;
	if (path && (strcmp(path, paths[1]) == 0 || strcmp(path, alias) == 0))
		return 1;
	return -1;
}

static int mock_stat(const char *path, struct stat *info)
{
	int index = device_index(path);
	if (index < 0 || missing_device)
		return -1;
	*info = (struct stat){.st_mode = regular_file ? S_IFREG : S_IFCHR,
			      .st_dev = 1,
			      .st_ino = 100 + (unsigned)index + inode_generation[index],
			      .st_rdev = 200 + (unsigned)index,
			      .st_ctime = 1};
	return 0;
}

static char *mock_realpath(const char *path, char *resolved)
{
	int index = device_index(path);
	if (index < 0)
		return NULL;
	strcpy(resolved, paths[index]);
	return resolved;
}

static int mock_glob(const char *pattern, int flags, int (*error)(const char *, int), glob_t *result)
{
	(void)flags;
	(void)error;
	CHECK(strcmp(pattern, "/dev/dri/renderD*") == 0);
	result->gl_pathc = 2;
	result->gl_pathv = paths;
	return 0;
}

static void mock_globfree(glob_t *result)
{
	(void)result;
}

static int mock_drm_device(dev_t id, uint32_t flags, drmDevicePtr *device)
{
	(void)flags;
	CHECK(id == 200 || id == 201);
	if (fail_metadata)
		return -1;
	static drmPciDeviceInfo pci[2] = {{.vendor_id = 0x10de}, {.vendor_id = 0x1002}};
	static drmDevice records[2];
	unsigned index = (unsigned)(id - 200);
	records[index].bustype = non_pci ? DRM_BUS_PLATFORM : DRM_BUS_PCI;
	records[index].deviceinfo.pci = &pci[index];
	*device = &records[index];
	return 0;
}

static void mock_drm_free(drmDevicePtr *device)
{
	*device = NULL;
}

static int mock_open(const char *path, int flags, ...)
{
	(void)flags;
	int index = device_index(path);
	CHECK(index >= 0);
	opens[index]++;
	return index + 10;
}

static int mock_close(int fd)
{
	CHECK(fd == 10 || fd == 11);
	closes[fd - 10]++;
	return 0;
}

static VADisplay mock_display(int fd)
{
	return fail_display ? NULL : (VADisplay)(uintptr_t)(fd - 9);
}

static unsigned display_index(VADisplay display)
{
	CHECK((uintptr_t)display == 1 || (uintptr_t)display == 2);
	return (unsigned)(uintptr_t)display - 1;
}

static VAStatus mock_initialize(VADisplay display, int *major, int *minor)
{
	initializations[display_index(display)]++;
	*major = 1;
	*minor = 24;
	return fail_initialize ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}

static VAStatus mock_terminate(VADisplay display)
{
	terminations[display_index(display)]++;
	return VA_STATUS_SUCCESS;
}

static VAStatus mock_driver(VADisplay display, char *driver)
{
	snprintf(driver_overrides[display_index(display)], 32, "%s", driver);
	return fail_driver ? VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}

static const char *mock_vendor(VADisplay display)
{
	return display_index(display) ? "Mesa radeonsi" : "NVIDIA NVDEC";
}

static VAMessageCallback mock_callback(VADisplay display, VAMessageCallback callback, void *context)
{
	(void)display;
	(void)callback;
	(void)context;
	return NULL;
}

static VAStatus mock_attributes(VADisplay display, VAProfile profile, VAEntrypoint entrypoint,
				VAConfigAttrib *attributes, int count)
{
	(void)entrypoint;
	CHECK(count == 1);
	if (display_index(display) == 0 || profile == VAProfileAV1Profile0)
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	attributes[0].value = VA_RC_CBR;
	return VA_STATUS_SUCCESS;
}

static uint64_t mock_time(void)
{
	return now_ns;
}

static int mock_hwdevice(AVBufferRef **reference, enum AVHWDeviceType type, const char *path, AVDictionary *options,
			 int flags)
{
	(void)reference;
	CHECK(type == AV_HWDEVICE_TYPE_VAAPI && flags == 0 && path);
	const AVDictionaryEntry *driver = av_dict_get(options, "driver", NULL, 0);
	if (expected_hwdriver)
		CHECK(driver && strcmp(driver->value, expected_hwdriver) == 0);
	else
		CHECK(!driver);
	hwdevice_calls++;
	return hwdevice_result;
}

/* Function-like stat replacement leaves the struct stat type untouched. The
 * shared DRM helper is first included by production code below these mocks. */
#define stat(...) mock_stat(__VA_ARGS__)
#define realpath mock_realpath
#define glob mock_glob
#define globfree mock_globfree
#define drmGetDeviceFromDevId mock_drm_device
#define drmFreeDevice mock_drm_free
#define open mock_open
#define close mock_close
#define vaGetDisplayDRM mock_display
#define vaInitialize mock_initialize
#define vaTerminate mock_terminate
#define vaSetDriverName mock_driver
#define vaQueryVendorString mock_vendor
#define vaSetInfoCallback mock_callback
#define vaSetErrorCallback mock_callback
#define vaGetConfigAttributes mock_attributes
#define os_gettime_ns mock_time
#define av_hwdevice_ctx_create mock_hwdevice
#include "../vaapi-utils.c"

static void test_metadata(void)
{
	CHECK(obs_drm_device_vendor(paths[0]) == OBS_DRM_VENDOR_NVIDIA);
	CHECK(obs_drm_device_vendor(alias) == OBS_DRM_VENDOR_AMD);
	CHECK(obs_drm_device_vendor(NULL) == OBS_DRM_VENDOR_UNKNOWN);
	fail_metadata = true;
	CHECK(obs_drm_device_vendor(paths[0]) == OBS_DRM_VENDOR_UNKNOWN);
	fail_metadata = false;
	non_pci = true;
	CHECK(obs_drm_device_vendor(paths[0]) == OBS_DRM_VENDOR_UNKNOWN);
	non_pci = false;
	regular_file = true;
	CHECK(obs_drm_device_vendor(paths[0]) == OBS_DRM_VENDOR_UNKNOWN);
	regular_file = false;
	CHECK(!obs_drm_may_support_qsv(OBS_DRM_VENDOR_AMD));
	CHECK(!obs_drm_may_support_qsv(OBS_DRM_VENDOR_NVIDIA));
	CHECK(obs_drm_may_support_qsv(OBS_DRM_VENDOR_INTEL));
	CHECK(obs_drm_may_support_qsv(OBS_DRM_VENDOR_UNKNOWN));
	CHECK(strcmp(obs_drm_vaapi_driver_override(OBS_DRM_VENDOR_INTEL, "nvidia"), "iHD") == 0);
	CHECK(!obs_drm_vaapi_driver_override(OBS_DRM_VENDOR_NVIDIA, "nvidia"));
	CHECK(!obs_drm_vaapi_driver_override(OBS_DRM_VENDOR_UNKNOWN, "nvidia"));
	CHECK(!obs_drm_vaapi_driver_override(OBS_DRM_VENDOR_INTEL, "i965"));
	CHECK(!obs_drm_vaapi_driver_override(OBS_DRM_VENDOR_AMD, NULL));
}

int main(void)
{
	CHECK(setenv("LIBVA_DRIVER_NAME", "nvidia", 1) == 0);
	test_metadata();
	AVBufferRef *reference = NULL;
	expected_hwdriver = "radeonsi";
	CHECK(vaapi_create_hwdevice(&reference, alias) == 0);
	expected_hwdriver = NULL;
	CHECK(vaapi_create_hwdevice(&reference, paths[0]) == 0);
	hwdevice_result = -123;
	CHECK(vaapi_create_hwdevice(&reference, paths[0]) == -123);
	CHECK(hwdevice_calls == 3);

	int fd = -1;
	VADisplay display = vaapi_open_device(&fd, alias, "driver selection test");
	CHECK(display && fd == 11);
	CHECK(strcmp(driver_overrides[1], "radeonsi") == 0);
	vaapi_close_device(&fd, display);
	CHECK(fd == -1);

	display = vaapi_open_device(&fd, paths[0], "NVIDIA selection test");
	CHECK(display && driver_overrides[0][0] == '\0');
	vaapi_close_device(&fd, display);

	fail_display = true;
	CHECK(!vaapi_open_device(&fd, paths[1], "failed display test"));
	CHECK(fd == -1 && opens[1] == closes[1]);
	fail_display = false;
	fail_driver = true;
	CHECK(!vaapi_open_device(&fd, paths[1], "failed driver test"));
	CHECK(fd == -1 && opens[1] == closes[1]);
	fail_driver = false;
	CHECK(opens[0] == closes[0] && opens[1] == closes[1]);
	CHECK(strcmp(getenv("LIBVA_DRIVER_NAME"), "nvidia") == 0);
	puts("GPU identity, per-device VA-API driver selection and cleanup checks passed");
	return 0;
}
