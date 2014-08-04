/**************************************************************************

libtbm_exynos4412

Copyright 2012 Samsung Electronics co., Ltd. All Rights Reserved.

Contact: SooChan Lim <sc1.lim@samsung.com>, Sangjin Lee <lsj119@samsung.com>

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
#include <xf86drm.h>
#include <tbm_bufmgr.h>
#include <tbm_bufmgr_backend.h>
#include "exynos_drm.h"
#include <pthread.h>

#define DEBUG
#define USE_DMAIMPORT
#define TBM_EXYNOS4412_LOG(...) fprintf (stderr, __VA_ARGS__)

#ifdef DEBUG
static int bDebug = 0;
#define DBG(...) if(bDebug&0x1) TBM_EXYNOS4412_LOG (__VA_ARGS__)
#else
#define DBG(...)
#endif

/* check condition */
#define EXYNOS4412_RETURN_IF_FAIL(cond) {\
    if (!(cond)) {\
        TBM_EXYNOS4412_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return;\
    }\
}
#define EXYNOS4412_RETURN_VAL_IF_FAIL(cond, val) {\
    if (!(cond)) {\
        TBM_EXYNOS4412_LOG ("[%s] : '%s' failed.\n", __FUNCTION__, #cond);\
        return val;\
    }\
}

struct dma_buf_info {
	unsigned long	size;
	unsigned int	fence_supported;
	unsigned int	padding;
};

#define DMA_BUF_ACCESS_READ		0x1
#define DMA_BUF_ACCESS_WRITE		0x2
#define DMA_BUF_ACCESS_DMA		0x4
#define DMA_BUF_ACCESS_MAX		0x8

#define DMA_FENCE_LIST_MAX 		5

struct dma_buf_fence {
	unsigned long		ctx;
	unsigned int		type;
};

#define DMABUF_IOCTL_BASE	'F'
#define DMABUF_IOWR(nr, type)	_IOWR(DMABUF_IOCTL_BASE, nr, type)

#define DMABUF_IOCTL_GET_INFO	DMABUF_IOWR(0x00, struct dma_buf_info)
#define DMABUF_IOCTL_GET_FENCE	DMABUF_IOWR(0x01, struct dma_buf_fence)
#define DMABUF_IOCTL_PUT_FENCE	DMABUF_IOWR(0x02, struct dma_buf_fence)

typedef struct _tbm_bufmgr_exynos4412 *tbm_bufmgr_exynos4412;
typedef struct _tbm_bo_exynos4412 *tbm_bo_exynos4412;

typedef struct _exynos4412_private
{
    int ref_count;
} PrivGem;

/* tbm buffor object for exynos4412 */
struct _tbm_bo_exynos4412
{
    int fd;

    unsigned int name;    /* FLINK ID */

    unsigned int gem;     /* GEM Handle */

    unsigned int dmabuf;  /* fd for dmabuf */

    void *pBase;          /* virtual address */

    unsigned int size;

    unsigned int flags_exynos;
    unsigned int flags_tbm;

    PrivGem* private;

    pthread_mutex_t mutex;
    struct dma_buf_fence dma_fence[DMA_FENCE_LIST_MAX];
    int device;
    int opt;
};

/* tbm bufmgr private for exynos4412 */
struct _tbm_bufmgr_exynos4412
{
    int fd;
    int isLocal;
    void* hashBos;

    int use_dma_fence;
};

char *STR_DEVICE[]=
{
    "DEF",
    "CPU",
    "2D",
    "3D",
    "MM"
};

char *STR_OPT[]=
{
    "NONE",
    "RD",
    "WR",
    "RDWR"
};

static unsigned int
_get_exynos_flag_from_tbm (unsigned int ftbm)
{
    unsigned int flags = 0;

    if (ftbm & TBM_BO_SCANOUT)
        flags |= EXYNOS_BO_CONTIG;
    else
        flags |= EXYNOS_BO_NONCONTIG;

    if (ftbm & TBM_BO_WC)
        flags |= EXYNOS_BO_WC;
    else if (ftbm & TBM_BO_NONCACHABLE)
        flags |= EXYNOS_BO_NONCACHABLE;
    else
        flags |= EXYNOS_BO_CACHABLE;

    return flags;
}

static unsigned int
_get_tbm_flag_from_exynos (unsigned int fexynos)
{
    unsigned int flags = 0;

    if (fexynos & EXYNOS_BO_NONCONTIG)
        flags |= TBM_BO_DEFAULT;
    else
        flags |= TBM_BO_SCANOUT;

    if (fexynos & EXYNOS_BO_WC)
        flags |= TBM_BO_WC;
    else if (fexynos & EXYNOS_BO_CACHABLE)
        flags |= TBM_BO_DEFAULT;
    else
        flags |= TBM_BO_NONCACHABLE;

    return flags;
}

static unsigned int
_get_name (int fd, unsigned int gem)
{
    struct drm_gem_flink arg = {0,};

    arg.handle = gem;
    if (drmIoctl (fd, DRM_IOCTL_GEM_FLINK, &arg))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d fail to get flink gem=%d\n",
                 getpid(), __FUNCTION__, __LINE__, gem);
        return 0;
    }

    return (unsigned int)arg.name;
}

static tbm_bo_handle
_exynos4412_bo_handle (tbm_bo_exynos4412 bo_exynos4412, int device)
{
    tbm_bo_handle bo_handle;
    memset (&bo_handle, 0x0, sizeof (uint64_t));

    switch(device)
    {
    case TBM_DEVICE_DEFAULT:
    case TBM_DEVICE_2D:
        bo_handle.u32 = (uint32_t)bo_exynos4412->gem;
        break;
    case TBM_DEVICE_CPU:
        if (!bo_exynos4412->pBase)
        {
            struct drm_exynos_gem_mmap arg = {0,};

            arg.handle = bo_exynos4412->gem;
            arg.size = bo_exynos4412->size;
            if (drmCommandWriteRead (bo_exynos4412->fd, DRM_EXYNOS_GEM_MMAP, &arg, sizeof(arg)))
            {
                TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                         "error %s:%d Cannot usrptr gem=%d\n",
                         getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos4412->pBase = (void*)((uint32_t)arg.mapped);
        }

        bo_handle.ptr = (void *)bo_exynos4412->pBase;
        break;
    case TBM_DEVICE_3D:
#ifdef USE_DMAIMPORT
        if (bo_exynos4412->dmabuf)
        {
            bo_handle.u32 = (uint32_t)bo_exynos4412->dmabuf;
            break;
        }

        if (!bo_exynos4412->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_exynos4412->gem;
            if (drmIoctl (bo_exynos4412->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                         "error %s:%d Cannot dmabuf=%d\n",
                         getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos4412->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_exynos4412->dmabuf;
#endif
        break;
    case TBM_DEVICE_MM:
        if (!bo_exynos4412->dmabuf)
        {
            struct drm_prime_handle arg = {0, };

            arg.handle = bo_exynos4412->gem;
            if (drmIoctl (bo_exynos4412->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
            {
                TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                         "error %s:%d Cannot dmabuf=%d\n",
                         getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
                return (tbm_bo_handle) NULL;
            }
            bo_exynos4412->dmabuf = arg.fd;
        }

        bo_handle.u32 = (uint32_t)bo_exynos4412->dmabuf;
        break;
    default:
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d Not supported device:%d\n",
                 getpid(), __FUNCTION__, __LINE__, device);
        bo_handle.ptr = (void *) NULL;
        break;
    }

    return bo_handle;
}

static int
_exynos4412_cache_flush (int fd, tbm_bo_exynos4412 bo_exynos4412, int flags)
{
#ifdef ENABLE_CACHECRTL
    struct drm_exynos_gem_cache_op cache_op = {0, };
    int ret;

    /* if bo_exynos4412 is null, do cache_flush_all */
    if(bo_exynos4412)
    {
        cache_op.flags = 0;
        cache_op.usr_addr = (uint64_t)((uint32_t)bo_exynos4412->pBase);
        cache_op.size = bo_exynos4412->size;
    }
    else
    {
        flags = TBM_CACHE_FLUSH_ALL;
        cache_op.flags = 0;
        cache_op.usr_addr = 0;
        cache_op.size = 0;
    }

    if (flags & TBM_CACHE_INV)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= EXYNOS_DRM_CACHE_INV_ALL;
        else
            cache_op.flags |= EXYNOS_DRM_CACHE_INV_RANGE;
    }

    if (flags & TBM_CACHE_CLN)
    {
        if(flags & TBM_CACHE_ALL)
            cache_op.flags |= EXYNOS_DRM_CACHE_CLN_ALL;
        else
            cache_op.flags |= EXYNOS_DRM_CACHE_CLN_RANGE;
    }

    if(flags & TBM_CACHE_ALL)
        cache_op.flags |= EXYNOS_DRM_ALL_CACHES_CORES;

    ret = drmCommandWriteRead (fd, DRM_EXYNOS_GEM_CACHE_OP, &cache_op, sizeof(cache_op));
    if (ret)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d fail to flush the cache.\n",
                 getpid(), __FUNCTION__, __LINE__);
        return 0;
    }
#else
    TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
             "warning %s:%d fail to enable the cache flush.\n",
             getpid(), __FUNCTION__, __LINE__);
#endif
    return 1;
}

static int
tbm_exynos4412_bo_size (tbm_bo bo)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);

    return bo_exynos4412->size;
}

static void *
tbm_exynos4412_bo_alloc (tbm_bo bo, int size, int flags)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;
    tbm_bufmgr_exynos4412 bufmgr_exynos4412;
    unsigned int exynos_flags;

    bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bufmgr_exynos4412!=NULL, 0);

    bo_exynos4412 = calloc (1, sizeof(struct _tbm_bo_exynos4412));
    if (!bo_exynos4412)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d fail to allocate the bo private\n",
                 getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    exynos_flags = _get_exynos_flag_from_tbm (flags);
    if((flags & TBM_BO_SCANOUT) &&
        size <= 4*1024)
    {
        exynos_flags |= EXYNOS_BO_NONCONTIG;
    }

    struct drm_exynos_gem_create arg = {0, };
    arg.size = size;
    arg.flags = exynos_flags;
    if (drmCommandWriteRead(bufmgr_exynos4412->fd, DRM_EXYNOS_GEM_CREATE, &arg, sizeof(arg)))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d Cannot create bo(flag:%x, size:%d)\n",
                 getpid(), __FUNCTION__, __LINE__, arg.flags, (unsigned int)arg.size);
        free (bo_exynos4412);
        return 0;
    }

    bo_exynos4412->fd = bufmgr_exynos4412->fd;
    bo_exynos4412->gem = arg.handle;
    bo_exynos4412->size = size;
    bo_exynos4412->flags_tbm = flags;
    bo_exynos4412->flags_exynos = exynos_flags;
    bo_exynos4412->name = _get_name (bo_exynos4412->fd, bo_exynos4412->gem);

    pthread_mutex_init(&bo_exynos4412->mutex, NULL);

    if (bufmgr_exynos4412->use_dma_fence
        && !bo_exynos4412->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_exynos4412->gem;
        if (drmIoctl (bo_exynos4412->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d Cannot dmabuf=%d\n",
                    getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
            free (bo_exynos4412);
            return 0;
        }
        bo_exynos4412->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem* privGem = calloc (1, sizeof(PrivGem));
    privGem->ref_count = 1;
    if (drmHashInsert(bufmgr_exynos4412->hashBos, bo_exynos4412->name, (void *)privGem) < 0)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d Cannot insert bo to Hash(%d)\n",
                 getpid(), __FUNCTION__, __LINE__, bo_exynos4412->name);
    }

    DBG ("[libtbm-exynos4412:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
         __FUNCTION__, bo_exynos4412->size,
         bo_exynos4412->gem, bo_exynos4412->name,
         flags, exynos_flags);

    return (void *)bo_exynos4412;
}

static void
tbm_exynos4412_bo_free(tbm_bo bo)
{
    tbm_bo_exynos4412 bo_exynos4412;
    tbm_bufmgr_exynos4412 bufmgr_exynos4412;

    if (!bo)
        return;

    bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS4412_RETURN_IF_FAIL (bufmgr_exynos4412!=NULL);

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_IF_FAIL (bo_exynos4412!=NULL);

    DBG ("[libtbm-exynos4412:%d] %s size:%d, gem:%d(%d)\n",
         getpid(), __FUNCTION__, bo_exynos4412->size, bo_exynos4412->gem, bo_exynos4412->name);

    if (bo_exynos4412->pBase)
    {
        if (munmap(bo_exynos4412->pBase, bo_exynos4412->size) == -1)
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                     "error %s:%d\n",
                     getpid(), __FUNCTION__, __LINE__);
        }
    }

    /* close dmabuf */
    if (bo_exynos4412->dmabuf)
    {
        close (bo_exynos4412->dmabuf);
        bo_exynos4412->dmabuf = 0;
    }

    /* delete bo from hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_exynos4412->hashBos, bo_exynos4412->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count--;
        if (privGem->ref_count == 0)
        {
            drmHashDelete (bufmgr_exynos4412->hashBos, bo_exynos4412->name);
            free (privGem);
            privGem = NULL;
        }
    }
    else
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "warning %s:%d Cannot find bo to Hash(%d), ret=%d\n",
                 getpid(), __FUNCTION__, __LINE__, bo_exynos4412->name, ret);
    }

    /* Free gem handle */
    struct drm_gem_close arg = {0, };
    memset (&arg, 0, sizeof(arg));
    arg.handle = bo_exynos4412->gem;
    if (drmIoctl (bo_exynos4412->fd, DRM_IOCTL_GEM_CLOSE, &arg))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                 "error %s:%d\n",
                 getpid(), __FUNCTION__, __LINE__);
    }

    free (bo_exynos4412);
}


static void *
tbm_exynos4412_bo_import (tbm_bo bo, unsigned int key)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_exynos4412 bufmgr_exynos4412;
    tbm_bo_exynos4412 bo_exynos4412;

    bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bufmgr_exynos4412!=NULL, 0);

    struct drm_gem_open arg = {0, };
    struct drm_exynos_gem_info info = {0, };

    arg.name = key;
    if (drmIoctl(bufmgr_exynos4412->fd, DRM_IOCTL_GEM_OPEN, &arg))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot open gem name=%d\n",
                getpid(), __FUNCTION__, __LINE__, key);
        return 0;
    }

    info.handle = arg.handle;
    if (drmCommandWriteRead(bufmgr_exynos4412->fd,
                           DRM_EXYNOS_GEM_GET,
                           &info,
                           sizeof(struct drm_exynos_gem_info)))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot get gem info=%d\n",
                getpid(), __FUNCTION__, __LINE__, key);
        return 0;
    }

    bo_exynos4412 = calloc (1, sizeof(struct _tbm_bo_exynos4412));
    if (!bo_exynos4412)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d fail to allocate the bo private\n",
                getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    bo_exynos4412->fd = bufmgr_exynos4412->fd;
    bo_exynos4412->gem = arg.handle;
    bo_exynos4412->size = arg.size;
    bo_exynos4412->flags_exynos = info.flags;
    bo_exynos4412->name = key;
    bo_exynos4412->flags_tbm = _get_tbm_flag_from_exynos (bo_exynos4412->flags_exynos);

    if (!bo_exynos4412->dmabuf)
    {
        struct drm_prime_handle arg = {0, };

        arg.handle = bo_exynos4412->gem;
        if (drmIoctl (bo_exynos4412->fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &arg))
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d Cannot dmabuf=%d\n",
                    getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
            free (bo_exynos4412);
            return 0;
        }
        bo_exynos4412->dmabuf = arg.fd;
    }

    /* add bo to hash */
    PrivGem *privGem = NULL;
    int ret;

    ret = drmHashLookup (bufmgr_exynos4412->hashBos, bo_exynos4412->name, (void**)&privGem);
    if (ret == 0)
    {
        privGem->ref_count++;
    }
    else if (ret == 1)
    {
        privGem = calloc (1, sizeof(PrivGem));
        privGem->ref_count = 1;
        if (drmHashInsert (bufmgr_exynos4412->hashBos, bo_exynos4412->name, (void *)privGem) < 0)
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d Cannot insert bo to Hash(%d)\n",
                    getpid(), __FUNCTION__, __LINE__, bo_exynos4412->name);
        }
    }
    else
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot insert bo to Hash(%d)\n",
                getpid(), __FUNCTION__, __LINE__, bo_exynos4412->name);
    }

    DBG ("[libtbm-exynos4412:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
         __FUNCTION__, bo_exynos4412->size,
         bo_exynos4412->gem, bo_exynos4412->name,
         bo_exynos4412->flags_tbm, bo_exynos4412->flags_exynos);

    return (void *)bo_exynos4412;
}

static unsigned int
tbm_exynos4412_bo_export (tbm_bo bo)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    if (!bo_exynos4412->name)
    {
        bo_exynos4412->name = _get_name(bo_exynos4412->fd, bo_exynos4412->gem);
        if (!bo_exynos4412->name)
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d Cannot get name\n",
                    getpid(), __FUNCTION__, __LINE__);
            return 0;
        }
    }

    DBG ("[libtbm-exynos4412:%d] %s size:%d, gem:%d(%d), flags:%d(%d)\n", getpid(),
         __FUNCTION__, bo_exynos4412->size,
         bo_exynos4412->gem, bo_exynos4412->name,
         bo_exynos4412->flags_tbm, bo_exynos4412->flags_exynos);

    return (unsigned int)bo_exynos4412->name;
}

static tbm_bo_handle
tbm_exynos4412_bo_get_handle (tbm_bo bo, int device)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, (tbm_bo_handle) NULL);

    if (!bo_exynos4412->gem)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot map gem=%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[libtbm-exynos4412:%d] %s gem:%d(%d), %s\n", getpid(),
         __FUNCTION__, bo_exynos4412->gem, bo_exynos4412->name, STR_DEVICE[device]);

    /*Get mapped bo_handle*/
    bo_handle = _exynos4412_bo_handle (bo_exynos4412, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot get handle: gem:%d, device:%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem, device);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static tbm_bo_handle
tbm_exynos4412_bo_map (tbm_bo bo, int device, int opt)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, (tbm_bo_handle) NULL);

    tbm_bo_handle bo_handle;
    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, (tbm_bo_handle) NULL);

    if (!bo_exynos4412->gem)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot map gem=%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem);
        return (tbm_bo_handle) NULL;
    }

    DBG ("[libtbm-exynos4412:%d] %s gem:%d(%d), %s, %s\n", getpid(),
         __FUNCTION__, bo_exynos4412->gem, bo_exynos4412->name, STR_DEVICE[device], STR_OPT[opt]);

    /*Get mapped bo_handle*/
    bo_handle = _exynos4412_bo_handle (bo_exynos4412, device);
    if (bo_handle.ptr == NULL)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d Cannot get handle: gem:%d, device:%d, opt:%d\n",
                getpid(), __FUNCTION__, __LINE__, bo_exynos4412->gem, device, opt);
        return (tbm_bo_handle) NULL;
    }

    return bo_handle;
}

static int
tbm_exynos4412_bo_unmap (tbm_bo bo)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    if (!bo_exynos4412->gem)
        return 0;

    DBG ("[libtbm-exynos4412:%d] %s gem:%d(%d) \n", getpid(),
         __FUNCTION__, bo_exynos4412->gem, bo_exynos4412->name);

    return 1;
}

static int
tbm_exynos4412_bo_cache_flush (tbm_bo bo, int flags)
{
    tbm_bufmgr_exynos4412 bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bufmgr_exynos4412!=NULL, 0);

    /* cache flush is managed by kernel side when using dma-fence. */
    if (bufmgr_exynos4412->use_dma_fence)
       return 1;

    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    if (!_exynos4412_cache_flush(bo_exynos4412->fd, bo_exynos4412, flags))
        return 0;

    return 1;
}

static int
tbm_exynos4412_bo_get_global_key (tbm_bo bo)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    if (!bo_exynos4412->name)
    {
        if (!bo_exynos4412->gem)
            return 0;

        bo_exynos4412->name = _get_name(bo_exynos4412->fd, bo_exynos4412->gem);
    }

    return bo_exynos4412->name;
}

static int
tbm_exynos4412_bo_lock(tbm_bo bo, int device, int opt)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bufmgr_exynos4412 bufmgr_exynos4412;
    tbm_bo_exynos4412 bo_exynos4412;
    struct dma_buf_fence fence;
    struct flock filelock;
    int ret=0;

    if (device != TBM_DEVICE_3D && device != TBM_DEVICE_CPU)
    {
        DBG ("[libtbm-exynos4412:%d] %s not support device type,\n", getpid(), __FUNCTION__);
	    return 0;
    }

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)tbm_backend_get_bufmgr_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bufmgr_exynos4412!=NULL, 0);

    memset(&fence, 0, sizeof(struct dma_buf_fence));

    /* Check if the given type is valid or not. */
    if (opt & TBM_OPTION_WRITE)
    {
        if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_WRITE | DMA_BUF_ACCESS_DMA;
    }
    else if (opt & TBM_OPTION_READ)
    {
        if (device == TBM_DEVICE_3D)
            fence.type = DMA_BUF_ACCESS_READ | DMA_BUF_ACCESS_DMA;
    }
    else
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] error %s:%d Invalid argument\n", getpid(), __FUNCTION__, __LINE__);
        return 0;
    }

    /* Check if the tbm manager supports dma fence or not. */
    if (!bufmgr_exynos4412->use_dma_fence)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                "error %s:%d  Not support DMA FENCE(%s)\n",
                getpid(), __FUNCTION__, __LINE__, strerror(errno) );
        return 0;

    }

    if (device == TBM_DEVICE_3D)
    {
        ret = ioctl(bo_exynos4412->dmabuf, DMABUF_IOCTL_GET_FENCE, &fence);
        if (ret < 0)
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d  Can not set GET FENCE(%s)\n",
                    getpid(), __FUNCTION__, __LINE__, strerror(errno) );
            return 0;
        }
    } else
    {
	if (opt & TBM_OPTION_WRITE)
	    filelock.l_type = F_WRLCK;
	else
	    filelock.l_type = F_RDLCK;

	filelock.l_whence = SEEK_CUR;
	filelock.l_start = 0;
	filelock.l_len = 0;

	if (-1 == fcntl(bo_exynos4412->dmabuf, F_SETLKW, &filelock))
        {
	    return 0;
	}
    }

    pthread_mutex_lock(&bo_exynos4412->mutex);

    if (device == TBM_DEVICE_3D)
    {
        int i;
        for (i = 0; i < DMA_FENCE_LIST_MAX; i++)
        {
            if (bo_exynos4412->dma_fence[i].ctx == 0)
            {
                bo_exynos4412->dma_fence[i].type = fence.type;
                bo_exynos4412->dma_fence[i].ctx = fence.ctx;
                break;
            }
        }

        if (i == DMA_FENCE_LIST_MAX)
        {
            //TODO: if dma_fence list is full, it needs realloc. I will fix this. by minseok3.kim
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d  fence list is full\n",
                    getpid(), __FUNCTION__, __LINE__);
        }
    }

    pthread_mutex_unlock(&bo_exynos4412->mutex);

    DBG ("[libtbm-exynos4412:%d] %s DMABUF_IOCTL_GET_FENCE! flink_id=%d dmabuf=%d\n", getpid(),
            __FUNCTION__, bo_exynos4412->name, bo_exynos4412->dmabuf);

    return 1;
}

static int
tbm_exynos4412_bo_unlock(tbm_bo bo)
{
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo!=NULL, 0);

    tbm_bo_exynos4412 bo_exynos4412;
    struct dma_buf_fence fence;
    struct flock filelock;
    unsigned int dma_type = 0;
    int ret=0;

    bo_exynos4412 = (tbm_bo_exynos4412)tbm_backend_get_bo_priv(bo);
    EXYNOS4412_RETURN_VAL_IF_FAIL (bo_exynos4412!=NULL, 0);

    if (bo_exynos4412->dma_fence[0].type & DMA_BUF_ACCESS_DMA)
	    dma_type = 1;

    if (!bo_exynos4412->dma_fence[0].ctx && dma_type)
    {
        DBG ("[libtbm-exynos4412:%d] %s FENCE not support or ignored,\n", getpid(), __FUNCTION__);
        return 0;
    }

    if (!bo_exynos4412->dma_fence[0].ctx && dma_type)
    {
        DBG ("[libtbm-exynos4412:%d] %s device type is not 3D/CPU,\n", getpid(), __FUNCTION__);
        return 0;
    }

    pthread_mutex_lock(&bo_exynos4412->mutex);

    if (dma_type)
    {
        fence.type = bo_exynos4412->dma_fence[0].type;
        fence.ctx = bo_exynos4412->dma_fence[0].ctx;
        int i;
        for (i = 1; i < DMA_FENCE_LIST_MAX; i++)
        {
            bo_exynos4412->dma_fence[i-1].type = bo_exynos4412->dma_fence[i].type;
            bo_exynos4412->dma_fence[i-1].ctx = bo_exynos4412->dma_fence[i].ctx;
        }
        bo_exynos4412->dma_fence[DMA_FENCE_LIST_MAX-1].type = 0;
        bo_exynos4412->dma_fence[DMA_FENCE_LIST_MAX-1].ctx = 0;
    }
    pthread_mutex_unlock(&bo_exynos4412->mutex);

    if (dma_type)
    {
        ret = ioctl(bo_exynos4412->dmabuf, DMABUF_IOCTL_PUT_FENCE, &fence);
        if (ret < 0)
        {
            TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] "
                    "error %s:%d  Can not set PUT FENCE(%s)\n",
                    getpid(), __FUNCTION__, __LINE__, strerror(errno) );
            return 0;
        }
    } else
    {
        filelock.l_type = F_UNLCK;
	filelock.l_whence = SEEK_CUR;
	filelock.l_start = 0;
	filelock.l_len = 0;

	if (-1 == fcntl(bo_exynos4412->dmabuf, F_SETLKW, &filelock))
        {
	    return 0;
	}
    }

    DBG ("[libtbm-exynos4412:%d] %s DMABUF_IOCTL_PUT_FENCE! flink_id=%d dmabuf=%d\n", getpid(),
            __FUNCTION__, bo_exynos4412->name, bo_exynos4412->dmabuf);

    return 1;
}

static void
tbm_exynos4412_bufmgr_deinit (void *priv)
{
    EXYNOS4412_RETURN_IF_FAIL (priv!=NULL);

    tbm_bufmgr_exynos4412 bufmgr_exynos4412;

    bufmgr_exynos4412 = (tbm_bufmgr_exynos4412)priv;

    if (bufmgr_exynos4412->hashBos)
    {
        unsigned long key;
        void *value;

        while (drmHashFirst(bufmgr_exynos4412->hashBos, &key, &value) > 0)
        {
            free (value);
            drmHashDelete (bufmgr_exynos4412->hashBos, key);
        }

        drmHashDestroy (bufmgr_exynos4412->hashBos);
        bufmgr_exynos4412->hashBos = NULL;
    }

    free (bufmgr_exynos4412);
}

MODULEINITPPROTO (init_tbm_bufmgr_priv);

static TBMModuleVersionInfo Exynos4412VersRec =
{
    "exynos4412",
    "Samsung",
    TBM_ABI_VERSION,
};

TBMModuleData tbmModuleData = { &Exynos4412VersRec, init_tbm_bufmgr_priv};

int
init_tbm_bufmgr_priv (tbm_bufmgr bufmgr, int fd)
{
    tbm_bufmgr_exynos4412 bufmgr_exynos4412;
    tbm_bufmgr_backend bufmgr_backend;

    if (!bufmgr)
        return 0;

    bufmgr_exynos4412 = calloc (1, sizeof(struct _tbm_bufmgr_exynos4412));
    if (!bufmgr_exynos4412)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] error: Fail to alloc bufmgr_exynos4412!\n", getpid());
        return 0;
    }

    bufmgr_exynos4412->fd = fd;
    if (bufmgr_exynos4412->fd < 0)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] error: Fail to create drm!\n", getpid());
        free (bufmgr_exynos4412);
        return 0;
    }

    //Create Hash Table
    bufmgr_exynos4412->hashBos = drmHashCreate ();

    //Check if the tbm manager supports dma fence or not.
    int fp = open("/sys/module/dmabuf_sync/parameters/enabled", O_RDONLY);
    int length;
    char buf[1];
    if (fp != -1)
    {
        length = read(fp, buf, 1);

        if (length == 1 && buf[0] == '1')
            bufmgr_exynos4412->use_dma_fence = 1;

        close(fp);
    }

    bufmgr_backend = tbm_backend_alloc();
    if (!bufmgr_backend)
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] error: Fail to create drm!\n", getpid());
        free (bufmgr_exynos4412);
        return 0;
    }

    bufmgr_backend->priv = (void *)bufmgr_exynos4412;
    bufmgr_backend->bufmgr_deinit = tbm_exynos4412_bufmgr_deinit,
    bufmgr_backend->bo_size = tbm_exynos4412_bo_size,
    bufmgr_backend->bo_alloc = tbm_exynos4412_bo_alloc,
    bufmgr_backend->bo_free = tbm_exynos4412_bo_free,
    bufmgr_backend->bo_import = tbm_exynos4412_bo_import,
    bufmgr_backend->bo_export = tbm_exynos4412_bo_export,
    bufmgr_backend->bo_get_handle = tbm_exynos4412_bo_get_handle,
    bufmgr_backend->bo_map = tbm_exynos4412_bo_map,
    bufmgr_backend->bo_unmap = tbm_exynos4412_bo_unmap,
    bufmgr_backend->bo_cache_flush = tbm_exynos4412_bo_cache_flush,
    bufmgr_backend->bo_get_global_key = tbm_exynos4412_bo_get_global_key;

    if (bufmgr_exynos4412->use_dma_fence)
    {
        bufmgr_backend->flags = (TBM_LOCK_CTRL_BACKEND | TBM_CACHE_CTRL_BACKEND);
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_lock2 = tbm_exynos4412_bo_lock;
        bufmgr_backend->bo_unlock = tbm_exynos4412_bo_unlock;
    }
    else
    {
        bufmgr_backend->flags = 0;
        bufmgr_backend->bo_lock = NULL;
        bufmgr_backend->bo_unlock = NULL;
    }

    if (!tbm_backend_init (bufmgr, bufmgr_backend))
    {
        TBM_EXYNOS4412_LOG ("[libtbm-exynos4412:%d] error: Fail to init backend!\n", getpid());
        tbm_backend_free (bufmgr_backend);
        free (bufmgr_exynos4412);
        return 0;
    }

#ifdef DEBUG
    {
        char* env;
        env = getenv ("TBM_EXYNOS4412_DEBUG");
        if (env)
        {
            bDebug = atoi (env);
            TBM_EXYNOS4412_LOG ("TBM_EXYNOS4412_DEBUG=%s\n", env);
        }
        else
        {
            bDebug = 0;
        }
    }
#endif

    DBG ("[libtbm-exynos4412:%d] %s DMABUF FENCE is %s\n", getpid(),
         __FUNCTION__, bufmgr_exynos4412->use_dma_fence ? "supported!" : "NOT supported!");

    DBG ("[libtbm-exynos4412:%d] %s fd:%d\n", getpid(),
         __FUNCTION__, bufmgr_exynos4412->fd);

    return 1;
}


