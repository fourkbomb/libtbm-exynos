/**************************************************************************

libtbm_android

Copyright 2016 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: Konstantin Drabeniuk <k.drabeniuk@samsung.com>,
		 Sergey Sizonov <s.sizonov@samsung.com>

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sub license, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice (including the
next paragraph) shall be included in all copies or substantial portions
of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

#include <tbm_bufmgr_backend.h>
#include <tbm_surface.h>
#include <dlog/dlog.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

static char *_target_name(void);

#define TBM_ANDROID_LOG(fmt, args...) LOGE("\033[31m"  "[%s]" fmt "\033[0m",\
										   _target_name(), ##args)

/* check condition */
#define ANDROID_RETURN_IF_FAIL(cond) {\
	if (!(cond)) {\
		TBM_ANDROID_LOG("[%s] : '%s' failed.\n", __func__, #cond);\
		return;\
	} \
}

#define ANDROID_RETURN_VAL_IF_FAIL(cond, val) {\
	if (!(cond)) {\
		TBM_ANDROID_LOG("[%s] : '%s' failed.\n", __func__, #cond);\
		return val;\
	} \
}

/* this macros has been copied from a gralloc implementation */
#define ALIGN(x, a)       (((x) + (a) - 1) & ~((a) - 1))

/*
 * Android to Tizen buffer formats map. (and vice versa)
 *
 * The formats set we currently support.
 * At this stage, we use formats that have a full match with Android formats.
 * In the future, we need to increase the number of matching formats.*/

#define ANDROID_TIZEN_FORMATS_MAP_SIZE 6

static const uint32_t android_tizen_formats_map[ANDROID_TIZEN_FORMATS_MAP_SIZE][2] =
{
	{ HAL_PIXEL_FORMAT_RGBA_8888, TBM_FORMAT_RGBA8888 },
	{ HAL_PIXEL_FORMAT_RGBX_8888, TBM_FORMAT_RGBX8888 },
	{ HAL_PIXEL_FORMAT_RGB_888,   TBM_FORMAT_RGB888 },
	{ HAL_PIXEL_FORMAT_RGB_565,   TBM_FORMAT_RGB565 },
	{ HAL_PIXEL_FORMAT_BGRA_8888, TBM_FORMAT_BGRA8888 },
	{ HAL_PIXEL_FORMAT_RGBA_4444, TBM_FORMAT_RGBA4444 }
};

/*
 * Android to Tizen flags map. (and vice versa)
 *
 *  - for the TBM_BO_SCANOUT flag we use the additional flag
 * GRALLOC_USAGE_HW_COMPOSER, because the buffer will be used by the
 * Hardware Composer.
 */

#define ANDROID_TIZEN_FLAGS_MAP_SIZE 2

static const uint32_t android_tizen_flags_map[ANDROID_TIZEN_FLAGS_MAP_SIZE][2] =
{
	{ GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN, TBM_BO_DEFAULT },
	{ GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN |
			GRALLOC_USAGE_PRIVATE_NONECACHE, TBM_BO_SCANOUT }
};


typedef struct _tbm_bufmgr_android *tbm_bufmgr_android;
typedef struct _tbm_bo_android *tbm_bo_android;

/* tbm buffer object for android */
struct _tbm_bo_android {
	buffer_handle_t handler;
	int width;
	int height;
	void *pBase;          /* virtual address */
	unsigned int map_cnt;
	unsigned int flags_tbm;
	uint32_t size;
};

/* tbm bufmgr private for android */
struct _tbm_bufmgr_android {
	const gralloc_module_t *gralloc_module;
	alloc_device_t *alloc_dev;
};


static char *
_target_name(void)
{
	FILE *f;
	char *slash;
	static int initialized = 0;
	static char app_name[128];

	if (initialized)
		return app_name;

	/* get the application name */
	f = fopen("/proc/self/cmdline", "r");

	if (!f)
		return 0;

	memset(app_name, 0x00, sizeof(app_name));

	if (fgets(app_name, 100, f) == NULL) {
		fclose(f);
		return 0;
	}

	fclose(f);

	slash = strrchr(app_name, '/');
	if (slash != NULL)
		memmove(app_name, slash + 1, strlen(slash));

	initialized = 1;

	return app_name;
}

/* @brief This function can be used to get the match for a @c value from the @c map table.
 *
 * @param[in]: map - the map to find the match in.
 * @param[in]: row_amount - the amount of rows of the map.
 * @param[in]: value - value you're looking the match for.
 * @param[in]: direction - the direction of 'matching', 0 - right to left, otherwise - left to right.
 * @return the match or -1 in an error case.
*/
static int32_t
_get_match(const uint32_t map[][2], uint32_t row_amount, uint32_t value, int direction)
{
	int i, direct;

	direct = direction ? 0 : 1;

	for (i = 0; i < row_amount; i++) {
		if (map[i][direct] == value)
			return map[i][direct ^ 1];
	}

	return -1;
}

static int
_get_android_format_from_tbm(unsigned int tbm_format)
{
	return _get_match(android_tizen_formats_map, ANDROID_TIZEN_FORMATS_MAP_SIZE, tbm_format, 0);
}

static int
_get_tbm_flags_from_android(int android_flags)
{
	return _get_match(android_tizen_flags_map, ANDROID_TIZEN_FLAGS_MAP_SIZE, android_flags, 1);
}

static int
_get_android_flags_from_tbm(int tbm_flags)
{
	return _get_match(android_tizen_flags_map, ANDROID_TIZEN_FLAGS_MAP_SIZE, tbm_flags, 0);
}

/**
 * @brief get the data of the surface.
 * @note Use NULL pointers on the components you're not interested
 * in: they'll be ignored by the function.
 * @param[in] width : the width of the surface
 * @param[in] height : the height of the surface
 * @param[in] android_format : the android_format of the surface
 * @param[out] size : the size of the surface
 * @param[out] pitch : the pitch of the surface
 * @return 1 if this function succeeds, otherwise 0.
 */
static int
_tbm_android_surface_get_data(int width, int height, int android_format,
							  uint32_t *size, uint32_t *pitch)
{
	/* function heavily inspired by the gralloc */
	/* bpp is bytes per pixel */
	size_t bpr;
	int bpp, vstride;

	uint32_t _size = 0;

	switch (android_format) {
	case HAL_PIXEL_FORMAT_RGBA_8888:
	case HAL_PIXEL_FORMAT_RGBX_8888:
	case HAL_PIXEL_FORMAT_BGRA_8888:
		bpp = 4;
		break;
	case HAL_PIXEL_FORMAT_RGB_888:
		bpp = 3;
		break;
	case HAL_PIXEL_FORMAT_RGB_565:
	case HAL_PIXEL_FORMAT_RGBA_4444:
		bpp = 2;
		break;
	default:
		return 0;
	}

	/* bpr is bytes per row */
	bpr = ALIGN(width*bpp, 64);
	vstride = ALIGN(height, 16);
	if (vstride < height + 2)
		_size = bpr * (height + 2);
	else
		_size = bpr * vstride;
	_size = ALIGN(_size, PAGE_SIZE);

	if (size) {
		*size = _size;
	}

	if (pitch) {
		*pitch = bpr;
	}

	return 1;
}

static tbm_bo_handle
_android_bo_handle(tbm_bufmgr_android bufmgr_android, tbm_bo_android bo_android,
				   int device)
{
	int ret, usage;
	tbm_bo_handle bo_handle;
	const gralloc_module_t *gralloc_module;

	gralloc_module = bufmgr_android->gralloc_module;
	ANDROID_RETURN_VAL_IF_FAIL(gralloc_module != NULL, (tbm_bo_handle) NULL);

	memset(&bo_handle, 0x0, sizeof(tbm_bo_handle));

	switch (device) {
	case TBM_DEVICE_DEFAULT:
	case TBM_DEVICE_2D:
		bo_handle.u64 = (uintptr_t)bo_android->handler;
		break;
	case TBM_DEVICE_CPU:
		if (!bo_android->pBase) {
			void *map = NULL;

			usage = GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN;

			ret = gralloc_module->lock(gralloc_module, bo_android->handler,
					usage, 0, 0, bo_android->width,
					bo_android->height, &map);
			if (ret || !map) {
				TBM_ANDROID_LOG("error(%s:%d): Cannot lock buffer\n",
								__func__, __LINE__);
				return (tbm_bo_handle) NULL;
			}

			bo_android->pBase = map;
		}
		bo_handle.ptr = bo_android->pBase;
		break;
	case TBM_DEVICE_3D:
	case TBM_DEVICE_MM:
	default:
		TBM_ANDROID_LOG("error: Not supported device:%d\n", device);
		bo_handle.ptr = NULL;
		break;
	}

	return bo_handle;
}

static void *
tbm_android_surface_bo_alloc(tbm_bo bo, int width, int height, int tbm_format,
							 int tbm_flags, int bo_idx)
{
	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, 0);

	int ret;
	tbm_bo_android bo_android;
	tbm_bufmgr_android bufmgr_android;
	int android_flags, android_format;
	alloc_device_t *alloc_dev;
	buffer_handle_t handler;
	int stride;
	uint32_t size;

	bufmgr_android = (tbm_bufmgr_android) tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bufmgr_android != NULL, 0);

	alloc_dev = bufmgr_android->alloc_dev;
	ANDROID_RETURN_VAL_IF_FAIL(alloc_dev != NULL, 0);

	bo_android = calloc(1, sizeof(struct _tbm_bo_android));
	if (!bo_android) {
		TBM_ANDROID_LOG("error: Fail to allocate the bo private\n");
		return 0;
	}

	android_flags  = _get_android_flags_from_tbm(tbm_flags);
	if (android_flags < 0) {
		TBM_ANDROID_LOG("error: this tbm(%d) -> android flag match isn't supported!", tbm_flags);
		free(bo_android);
		return 0;
	}

	android_format = _get_android_format_from_tbm(tbm_format);
	if (android_format < 0) {
		TBM_ANDROID_LOG("error: this tbm(%d) -> android format match isn't supported!", tbm_format);
		free(bo_android);
		return 0;
	}

	ret = alloc_dev->alloc(alloc_dev, width, height, android_format,
			android_flags, &handler, &stride);
	if (ret) {
		TBM_ANDROID_LOG
			("error: Cannot allocate a buffer(%dx%d) in graphic memory\n",
			 width, height);
		free(bo_android);
		return 0;
	}

	ret = _tbm_android_surface_get_data(width, height, android_format, &size, NULL);
	if (!ret) {
		TBM_ANDROID_LOG
			("error: Cannot get surface data\n");
		free(bo_android);
		return 0;
	}

	bo_android->handler = handler;
	bo_android->width = width;
	bo_android->height = height;
	bo_android->flags_tbm = tbm_flags;
	bo_android->size = size;

	return (void *)bo_android;
}

static void *
tbm_android_import(tbm_bo bo, const void *native)
{
	tbm_bufmgr_android bufmgr_android;
	const native_handle_t* native_handle;
	tbm_bo_android bo_android;

	int tbm_flags, android_format, android_flags;
	int width, height;
	uint32_t size;
	int ret;

	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, NULL);
	ANDROID_RETURN_VAL_IF_FAIL(native != NULL, NULL);

	bufmgr_android = (tbm_bufmgr_android)tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bufmgr_android != NULL, NULL);

	native_handle = native;
	ret = bufmgr_android->gralloc_module->registerBuffer(bufmgr_android->gralloc_module, native_handle);
	if (ret)
		return NULL;

	bo_android = calloc(1, sizeof(struct _tbm_bo_android));
	if (!bo_android) {
		TBM_ANDROID_LOG("error bo:%p fail to allocate the bo private\n", bo);
		return NULL;
	}

	/*
	 * TODO: must be confirmed by some documentation
	 *
	 * data[numFds + 4] = usage (flags) (access type)
	 * data[numFds + 5] = width
	 * data[numFds + 6] = height
	 * data[numFds + 7] = format
	 */
	android_flags = native_handle->data[native_handle->numFds + 4];
	width = native_handle->data[native_handle->numFds + 5];
	height = native_handle->data[native_handle->numFds + 6];
	android_format = native_handle->data[native_handle->numFds + 7];

	tbm_flags  = _get_tbm_flags_from_android(android_flags);
	if (tbm_flags < 0) {
		TBM_ANDROID_LOG("error: this android(%d) -> tbm flag match isn't supported!", android_flags);
		free(bo_android);
		return 0;
	}

	ret = _tbm_android_surface_get_data(width, height, android_format, &size, NULL);
	if (!ret) {
		TBM_ANDROID_LOG("error: Cannot get surface data\n");
		free(bo_android);
		return 0;
	}

	bo_android->handler = native_handle;
	bo_android->width = width;
	bo_android->height = height;
	bo_android->flags_tbm = tbm_flags;
	bo_android->size = size;

	return bo_android;
}

static const void *
tbm_android_export(tbm_bo bo)
{
	tbm_bo_android bo_android;

	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, NULL);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, NULL);

	return bo_android->handler;
}

static int
tbm_android_bo_size(tbm_bo bo)
{
	tbm_bo_android bo_android;

	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, 0);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, 0);

	return bo_android->size;
}

static void
tbm_android_bo_free(tbm_bo bo)
{
	tbm_bo_android bo_android;
	tbm_bufmgr_android bufmgr_android;

	if (!bo)
		return;

	bufmgr_android = (tbm_bufmgr_android) tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_IF_FAIL(bufmgr_android != NULL);

	bo_android = (tbm_bo_android) tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_IF_FAIL(bo_android != NULL);

	bufmgr_android->alloc_dev->free(bufmgr_android->alloc_dev,
									bo_android->handler);

	free(bo_android);
}

static tbm_bo_handle
tbm_android_bo_get_handle(tbm_bo bo, int device)
{
	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, (tbm_bo_handle) NULL);

	tbm_bo_handle bo_handle;
	tbm_bo_android bo_android;
	tbm_bufmgr_android bufmgr_android;

	bufmgr_android = (tbm_bufmgr_android) tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bufmgr_android != NULL, (tbm_bo_handle) NULL);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, (tbm_bo_handle) NULL);

	/*Get mapped bo_handle*/
	bo_handle = _android_bo_handle(bufmgr_android, bo_android, device);
	if (bo_handle.ptr == NULL) {
		TBM_ANDROID_LOG("error: Cannot get handle: device:%d\n", device);
		return (tbm_bo_handle) NULL;
	}

	return bo_handle;
}

static tbm_bo_handle
tbm_android_bo_map(tbm_bo bo, int device, int opt)
{
	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, (tbm_bo_handle) NULL);

	tbm_bo_handle bo_handle;
	tbm_bo_android bo_android;
	tbm_bufmgr_android bufmgr_android;

	bufmgr_android = (tbm_bufmgr_android) tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bufmgr_android != NULL, (tbm_bo_handle) NULL);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, (tbm_bo_handle) NULL);

	/*Get mapped bo_handle*/
	bo_handle = _android_bo_handle(bufmgr_android, bo_android, device);
	if (bo_handle.ptr == NULL) {
		TBM_ANDROID_LOG("error: Cannot get handle: device:%d\n", device);
		return (tbm_bo_handle) NULL;
	}

	bo_android->map_cnt++;

	return bo_handle;
}

static int
tbm_android_bo_unmap(tbm_bo bo)
{
	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, 0);

	int ret;
	tbm_bo_android bo_android;
	tbm_bufmgr_android bufmgr_android;
	const gralloc_module_t *gralloc_module;

	bufmgr_android = (tbm_bufmgr_android)tbm_backend_get_bufmgr_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bufmgr_android != NULL, 0);

	gralloc_module = bufmgr_android->gralloc_module;
	ANDROID_RETURN_VAL_IF_FAIL(gralloc_module != NULL, 0);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, 0);

	if (!bo_android->map_cnt) {
		TBM_ANDROID_LOG("error(%s:%d): The buffer is not mapped\n",
												__func__, __LINE__);
		return 0;
	}

	bo_android->map_cnt--;

	if (bo_android->map_cnt)
		return 1;

	ret = gralloc_module->unlock(gralloc_module, bo_android->handler);
	if (ret) {
		TBM_ANDROID_LOG("error(%s:%d): Cannot unlock buffer\n",
										__func__, __LINE__);
		return 0;
	}

	bo_android->pBase = NULL;

	return 1;
}

static void
tbm_android_bufmgr_deinit(void *priv)
{
	ANDROID_RETURN_IF_FAIL(priv != NULL);

	tbm_bufmgr_android bufmgr_android;

	bufmgr_android = (tbm_bufmgr_android) priv;

	gralloc_close(bufmgr_android->alloc_dev);

	free(bufmgr_android);
}

static int
tbm_android_surface_supported_format(uint32_t **formats, uint32_t *num)
{
	uint32_t *color_formats = NULL;
	int i;

	ANDROID_RETURN_VAL_IF_FAIL(formats != NULL, 0);
	ANDROID_RETURN_VAL_IF_FAIL(num != NULL, 0);

	color_formats = (uint32_t *)calloc(ANDROID_TIZEN_FORMATS_MAP_SIZE,
									   sizeof(uint32_t));

	if (color_formats == NULL)
		return 0;

	for (i = 0; i < ANDROID_TIZEN_FORMATS_MAP_SIZE; i++)
		color_formats[i] = android_tizen_formats_map[i][1];

	*formats = color_formats;
	*num = ANDROID_TIZEN_FORMATS_MAP_SIZE;

	return 1;
}

/**
 * @brief get the plane data of the surface.
 * @param[in] width : the width of the surface
 * @param[in] height : the height of the surface
 * @param[in] format : the format of the surface
 * @param[in] plane_idx : the format of the surface
 * @param[out] size : the size of the plane
 * @param[out] offset : the offset of the plane
 * @param[out] pitch : the pitch of the plane
 * @param[out] padding : the padding of the plane
 * @return 1 if this function succeeds, otherwise 0.
 */
static int
tbm_android_surface_get_plane_data(int width, int height,
				  tbm_format tbm_format, int plane_idx, uint32_t *size, uint32_t *offset,
				  uint32_t *pitch, int *bo_idx)
{
	int ret, android_format;

	/* As we use for allocate the buffer libgralloc, the offset and bo_idx is 0. */
	if (offset) {
		*offset = 0;
	}
	if (bo_idx) {
		*bo_idx = 0;
	}

	android_format = _get_android_format_from_tbm(tbm_format);
	if (android_format < 0) {
		TBM_ANDROID_LOG("error: this tbm(%d) -> android format match isn't supported!", tbm_format);
		return 0;
	}

	ret = _tbm_android_surface_get_data(width, height, android_format, size, pitch);

	return ret;
}

static int
tbm_android_bo_get_flags(tbm_bo bo)
{
	tbm_bo_android bo_android;

	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, 0);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, 0);

	return bo_android->flags_tbm;
}

static int
init_tbm_bufmgr_priv(tbm_bufmgr bufmgr, int fd)
{
	int ret;
	tbm_bufmgr_android bufmgr_android;
	tbm_bufmgr_backend bufmgr_backend;

	if (!bufmgr)
		return 0;

	bufmgr_android = calloc(1, sizeof(struct _tbm_bufmgr_android));
	if (!bufmgr_android) {
		TBM_ANDROID_LOG("error: Fail to alloc bufmgr_android!\n");
		return 0;
	}

	ret =
		hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
					  (const hw_module_t **)&bufmgr_android->gralloc_module);
	if (ret || !bufmgr_android->gralloc_module) {
		TBM_ANDROID_LOG("error: Cannot get gralloc hardware module!\n");
		goto fail_1;
	}

	ret =
		gralloc_open((const hw_module_t *)bufmgr_android->gralloc_module,
					 &bufmgr_android->alloc_dev);
	if (ret || !&bufmgr_android->alloc_dev) {
		TBM_ANDROID_LOG("error: Cannot open the gralloc!\n");
		goto fail_1;
	}

	bufmgr_backend = tbm_backend_alloc();
	if (!bufmgr_backend) {
		TBM_ANDROID_LOG("error: Fail to create android backend!\n");
		goto fail_2;
	}

	bufmgr_backend->flags = 0;
	bufmgr_backend->priv = (void *)bufmgr_android;
	bufmgr_backend->bufmgr_deinit = tbm_android_bufmgr_deinit;
	bufmgr_backend->bo_size = tbm_android_bo_size;

	/* we don't implement bo_alloc due to restriction of underlying
	 * functionality (gralloc). Gralloc doesn't allow to allocate bo without
	 * the specification of the buffer's height and width.*/
	bufmgr_backend->bo_alloc = NULL;
	bufmgr_backend->bo_free = tbm_android_bo_free;

	/* Android doesn't provide ability to share buffer across key */
	bufmgr_backend->bo_import = NULL;
	bufmgr_backend->bo_export = NULL;

	bufmgr_backend->bo_get_handle = tbm_android_bo_get_handle;
	bufmgr_backend->bo_map = tbm_android_bo_map;
	bufmgr_backend->bo_unmap = tbm_android_bo_unmap;

	/* Android provides an itself lock/unlock mechanism, look at tbm_android_bo_map */
	bufmgr_backend->bo_unlock = NULL;
	bufmgr_backend->bo_lock = NULL;

	bufmgr_backend->surface_supported_format =
										tbm_android_surface_supported_format;
	bufmgr_backend->surface_get_plane_data = tbm_android_surface_get_plane_data;

	/* Android doesn't provide ability to share buffer across fd */
	bufmgr_backend->bo_import_fd = NULL;
	bufmgr_backend->bo_export_fd = NULL;

	bufmgr_backend->bo_get_flags = tbm_android_bo_get_flags;
	bufmgr_backend->bufmgr_bind_native_display = NULL;
	bufmgr_backend->surface_bo_alloc = tbm_android_surface_bo_alloc;
	bufmgr_backend->bo_import_ = tbm_android_import;
	bufmgr_backend->bo_export_ = tbm_android_export;

	if (!tbm_backend_init(bufmgr, bufmgr_backend)) {
		TBM_ANDROID_LOG("error: Fail to init backend!\n");
		tbm_backend_free(bufmgr_backend);

		goto fail_2;
	}

	return 1;

fail_2:
	gralloc_close(bufmgr_android->alloc_dev);
fail_1:
	free(bufmgr_android);

	return 0;
}

static const TBMModuleVersionInfo android_vers = {
	"tbm-android",
	"Samsung",
	TBM_ABI_VERSION
};

const TBMModuleData tbmModuleData = { (TBMModuleVersionInfo *)&android_vers, init_tbm_bufmgr_priv };
