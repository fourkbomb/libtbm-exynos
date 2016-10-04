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
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <tbm_bufmgr.h>
#include <tbm_bufmgr_backend.h>
#include <pthread.h>
#include <tbm_surface.h>
#include <tbm_surface_internal.h>
#include <dlog/dlog.h>

#include <hardware/hardware.h>
#include <hardware/gralloc.h>

#define ALIGN(x, a)       (((x) + (a) - 1) & ~((a) - 1))

#define TBM_COLOR_FORMAT_COUNT (6)

uint32_t tbm_android_color_format_list[TBM_COLOR_FORMAT_COUNT] = {
												TBM_FORMAT_RGBA8888,
												TBM_FORMAT_RGBX8888,
												TBM_FORMAT_RGB888,
												TBM_FORMAT_RGB565,
												TBM_FORMAT_BGRA8888,
												TBM_FORMAT_RGBA4444
											};

char *target_name()
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

#define TBM_ANDROID_LOG(fmt, args...) LOGE("\033[31m"  "[%s]" fmt "\033[0m",\
										   target_name(), ##args)

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
	gralloc_module_t *gralloc_module;
	alloc_device_t *alloc_dev;
};

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
	gralloc_module_t *gralloc_module;

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

			ret = gralloc_module->lock((gralloc_module_t const *) gralloc_module,
					bo_android->handler, usage, 0, 0, bo_android->width,
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

/* At this stage, we use formats that have a full match with Android formats.
 * In the future, we need to increase the number of matching formats.*/
static int
_get_android_format_from_tbm(unsigned int tbm_format)
{
	int android_format = 0;

	switch (tbm_format) {
	case TBM_FORMAT_RGBA8888:
		android_format = HAL_PIXEL_FORMAT_RGBA_8888;
		break;
	case TBM_FORMAT_RGBX8888:
		android_format = HAL_PIXEL_FORMAT_RGBX_8888;
		break;
	case TBM_FORMAT_RGB888:
		android_format = HAL_PIXEL_FORMAT_RGB_888;
		break;
	case TBM_FORMAT_RGB565:
		android_format = HAL_PIXEL_FORMAT_RGB_565;
		break;
	case TBM_FORMAT_BGRA8888:
		android_format = HAL_PIXEL_FORMAT_BGRA_8888;
		break;
	case TBM_FORMAT_RGBA4444:
		android_format = HAL_PIXEL_FORMAT_RGBA_4444;
		break;
	}

	return android_format;
}

/*
 * TODO: think whether we able to create some 'flags match table'
 */
static int
_get_tbm_flags_from_android(int android_flags)
{
	int tbm_flags = -1;

	if (android_flags == (GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN))
		tbm_flags = TBM_BO_DEFAULT;
	else if (android_flags == (GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_PRIVATE_NONECACHE))
		tbm_flags = TBM_BO_SCANOUT;

	return tbm_flags;
}

/* For the TBM_BO_SCANOUT flag we use the additional flag
 * GRALLOC_USAGE_HW_COMPOSER, because the buffer will be used by the
 * Hardware Composer.*/
static int
_get_android_flags_from_tbm(int tbm_flags)
{
	int android_flags = -1;

	/* TODO: while TBM_BO_DEFAULT equals 0(zero) it's useless to use logical operations
	 * I'm a little bit confused..., maybe TBM_BO_DEFAULT must be 1 << 0 ?
	 * So, at least now, we'll use equal operator instead logical OR operator
	 * I think we must allow a read operation for the default bo. */
	if (tbm_flags == TBM_BO_DEFAULT)
		android_flags = GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_SW_READ_OFTEN;
	else if (tbm_flags == TBM_BO_SCANOUT)
		android_flags = GRALLOC_USAGE_HW_COMPOSER | GRALLOC_USAGE_SW_WRITE_OFTEN | GRALLOC_USAGE_PRIVATE_NONECACHE;

	return android_flags;
}

void *
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
	android_format = _get_android_format_from_tbm(tbm_format);

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
tbm_android_import(tbm_bo bo, void *native)
{
	tbm_bufmgr_android bufmgr_android;
	native_handle_t* native_handle;
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
	gralloc_module_t *gralloc_module;

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

	ret = gralloc_module->unlock((gralloc_module_t const*) gralloc_module,
								  bo_android->handler);
	if (ret) {
		TBM_ANDROID_LOG("error(%s:%d): Cannot unlock buffer\n",
										__func__, __LINE__);
		return 0;
	}

	bo_android->pBase = NULL;

	return 1;
}

static int
tbm_android_bo_lock(tbm_bo bo, int device, int opt)
{
	return 1;
}

static int
tbm_android_bo_unlock(tbm_bo bo)
{
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

int
tbm_android_surface_supported_format(uint32_t **formats, uint32_t *num)
{
	uint32_t *color_formats = NULL;

	ANDROID_RETURN_VAL_IF_FAIL(formats != NULL, 0);
	ANDROID_RETURN_VAL_IF_FAIL(num != NULL, 0);

	color_formats = (uint32_t *)calloc(TBM_COLOR_FORMAT_COUNT,
									   sizeof(uint32_t));

	if (color_formats == NULL)
		return 0;

	memcpy(color_formats, tbm_android_color_format_list,
		   sizeof(uint32_t) * TBM_COLOR_FORMAT_COUNT);

	*formats = color_formats;
	*num = TBM_COLOR_FORMAT_COUNT;

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
int
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
	ret = _tbm_android_surface_get_data(width, height, android_format, size, pitch);

	return ret;
}

int
tbm_android_bo_get_flags(tbm_bo bo)
{
	tbm_bo_android bo_android;

	ANDROID_RETURN_VAL_IF_FAIL(bo != NULL, 0);

	bo_android = (tbm_bo_android)tbm_backend_get_bo_priv(bo);
	ANDROID_RETURN_VAL_IF_FAIL(bo_android != NULL, 0);

	return bo_android->flags_tbm;
}

int
tbm_android_bufmgr_bind_native_display(tbm_bufmgr bufmgr, void *native_display)
{
	return 1;
}

MODULEINITPPROTO(init_tbm_bufmgr_priv);

static TBMModuleVersionInfo AndroidVersRec = {
	"android",
	"Samsung",
	TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &AndroidVersRec, init_tbm_bufmgr_priv };

int
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
		free(bufmgr_android);
		return 0;
	}

	ret =
		gralloc_open((const hw_module_t *)bufmgr_android->gralloc_module,
					 &bufmgr_android->alloc_dev);
	if (ret || !&bufmgr_android->alloc_dev) {
		printf("error: Cannot open the gralloc\n");
		free(bufmgr_android);
		return 0;
	}

	bufmgr_backend = tbm_backend_alloc();
	if (!bufmgr_backend) {
		TBM_ANDROID_LOG("error: Fail to create android backend!\n");

		gralloc_close(bufmgr_android->alloc_dev);

		free(bufmgr_android);
		return 0;
	}

	bufmgr_backend->priv = (void *)bufmgr_android;
	bufmgr_backend->bufmgr_deinit = tbm_android_bufmgr_deinit;
	bufmgr_backend->bo_size = tbm_android_bo_size;
	bufmgr_backend->bo_free = tbm_android_bo_free;
	bufmgr_backend->bo_get_handle = tbm_android_bo_get_handle;
	bufmgr_backend->bo_map = tbm_android_bo_map;
	bufmgr_backend->bo_unmap = tbm_android_bo_unmap;
	bufmgr_backend->surface_get_plane_data = tbm_android_surface_get_plane_data;
	bufmgr_backend->surface_supported_format =
										tbm_android_surface_supported_format;
	bufmgr_backend->bo_get_flags = tbm_android_bo_get_flags;
	bufmgr_backend->bo_lock = tbm_android_bo_lock;
	bufmgr_backend->bo_unlock = tbm_android_bo_unlock;
	/* we don't implement bo_alloc due to restriction of underlying
	 * functionality (gralloc). Gralloc not allow to allocate bo is not
	 * knowing the height and width of the buffer.*/
	bufmgr_backend->surface_bo_alloc = tbm_android_surface_bo_alloc;

	bufmgr_backend->bo_import_ = tbm_android_import;
/*	if (tbm_backend_is_display_server() && !_check_render_node()) {
		bufmgr_backend->bufmgr_bind_native_display = tbm_android_bufmgr_bind_native_display;
	}
*/
	if (!tbm_backend_init(bufmgr, bufmgr_backend)) {
		TBM_ANDROID_LOG("error: Fail to init backend!\n");
		tbm_backend_free(bufmgr_backend);

		gralloc_close(bufmgr_android->alloc_dev);

		free(bufmgr_android);
		return 0;
	}

	return 1;
}


