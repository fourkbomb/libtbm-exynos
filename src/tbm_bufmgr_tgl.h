/**************************************************************************
 *
 * libtbm
 *
 * Copyright 2012 Samsung Electronics co., Ltd. All Rights Reserved.
 *
 * Contact: SooChan Lim <sc1.lim@samsung.com>, Sangjin Lee <lsj119@samsung.com>
 * Boram Park <boram1288.park@samsung.com>, Changyeon Lee <cyeon.lee@samsung.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL PRECISION INSIGHT AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * **************************************************************************/

#ifndef __TBM_BUFMGR_TGL_H__
#define __TBM_BUFMGR_TGL_H__

#include <linux/ioctl.h>

#ifdef ENABLE_CACHECRTL
static char tgl_devfile[] = "/dev/slp_global_lock";
static char tgl_devfile1[] = "/dev/tgl";
#endif

#define TGL_IOC_BASE				0x32

struct tgl_attribute {
	unsigned int key;
	unsigned int timeout_ms;
};

struct tgl_user_data {
	unsigned int key;
	unsigned int data1;
	unsigned int data2;
	unsigned int locked;
};

typedef enum {
	_TGL_INIT_LOCK = 1,
	_TGL_DESTROY_LOCK,
	_TGL_LOCK_LOCK,
	_TGL_UNLOCK_LOCK,
	_TGL_SET_DATA,
	_TGL_GET_DATA,
} _tgl_ioctls;

#define TGL_IOC_INIT_LOCK			_IOW(TGL_IOC_BASE, _TGL_INIT_LOCK, struct tgl_attribute *)
#define TGL_IOC_DESTROY_LOCK		_IOW(TGL_IOC_BASE, _TGL_DESTROY_LOCK, unsigned int)
#define TGL_IOC_LOCK_LOCK			_IOW(TGL_IOC_BASE, _TGL_LOCK_LOCK, unsigned int)
#define TGL_IOC_UNLOCK_LOCK			_IOW(TGL_IOC_BASE, _TGL_UNLOCK_LOCK, unsigned int)
#define TGL_IOC_SET_DATA			_IOW(TGL_IOC_BASE, _TGL_SET_DATA, struct tgl_user_data *)
#define TGL_IOC_GET_DATA			_IOW(TGL_IOC_BASE, _TGL_GET_DATA, struct tgl_user_data *)

#ifdef ENABLE_CACHECRTL

/* indicate cache units. */
enum e_drm_exynos_gem_cache_sel {
	EXYNOS_DRM_L1_CACHE		= 1 << 0,
	EXYNOS_DRM_L2_CACHE		= 1 << 1,
	EXYNOS_DRM_ALL_CORES		= 1 << 2,
	EXYNOS_DRM_ALL_CACHES		= EXYNOS_DRM_L1_CACHE |
						EXYNOS_DRM_L2_CACHE,
	EXYNOS_DRM_ALL_CACHES_CORES	= EXYNOS_DRM_L1_CACHE |
						EXYNOS_DRM_L2_CACHE |
						EXYNOS_DRM_ALL_CORES,
	EXYNOS_DRM_CACHE_SEL_MASK	= EXYNOS_DRM_ALL_CACHES_CORES
};

/* indicate cache operation types. */
enum e_drm_exynos_gem_cache_op {
	EXYNOS_DRM_CACHE_INV_ALL	= 1 << 3,
	EXYNOS_DRM_CACHE_INV_RANGE	= 1 << 4,
	EXYNOS_DRM_CACHE_CLN_ALL	= 1 << 5,
	EXYNOS_DRM_CACHE_CLN_RANGE	= 1 << 6,
	EXYNOS_DRM_CACHE_FSH_ALL	= EXYNOS_DRM_CACHE_INV_ALL |
						EXYNOS_DRM_CACHE_CLN_ALL,
	EXYNOS_DRM_CACHE_FSH_RANGE	= EXYNOS_DRM_CACHE_INV_RANGE |
						EXYNOS_DRM_CACHE_CLN_RANGE,
	EXYNOS_DRM_CACHE_OP_MASK	= EXYNOS_DRM_CACHE_FSH_ALL |
						EXYNOS_DRM_CACHE_FSH_RANGE
};

/**
 * A structure for cache operation.
 *
 * @usr_addr: user space address.
 *	P.S. it SHOULD BE user space.
 * @size: buffer size for cache operation.
 * @flags: select cache unit and cache operation.
 * @gem_handle: a handle to a gem object.
 *	this gem handle is needed for cache range operation to L2 cache.
 */
struct drm_exynos_gem_cache_op {
	uint64_t usr_addr;
	unsigned int size;
	unsigned int flags;
	unsigned int gem_handle;
};

#define DRM_EXYNOS_GEM_CACHE_OP		0x12

#define DRM_IOCTL_EXYNOS_GEM_CACHE_OP  DRM_IOWR(DRM_COMMAND_BASE + \
		DRM_EXYNOS_GEM_CACHE_OP, struct drm_exynos_gem_cache_op)

#endif

#endif							/* __TBM_BUFMGR_TGL_H__ */
