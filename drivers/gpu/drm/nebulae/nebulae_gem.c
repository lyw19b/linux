// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae GEM/BO management.
 */

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mode.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>

#include "nebulae_internal.h"

static const struct drm_gem_object_funcs nebulae_gem_object_funcs;
static int nebulae_bo_wait_resv(struct nebulae_bo *bo,
				enum dma_resv_usage usage);

struct drm_gem_object *nebulae_gpu_gem_create_object(struct drm_device *drm,
						     size_t size)
{
	struct nebulae_bo *bo;

	if (!size)
		return ERR_PTR(-EINVAL);

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(&bo->link);
	bo->base.base.funcs = &nebulae_gem_object_funcs;
	return &bo->base.base;
}

static void nebulae_gem_object_free(struct drm_gem_object *obj)
{
	struct nebulae_device *ndev = to_nebulae(obj->dev);
	struct nebulae_bo *bo = to_nebulae_bo(obj);

	mutex_lock(&ndev->bo_lock);
	if (bo->listed) {
		list_del_init(&bo->link);
		bo->listed = false;
	}
	mutex_unlock(&ndev->bo_lock);

	nebulae_free_bo_va(ndev, bo);
	drm_gem_shmem_object_free(obj);
}

static void nebulae_gem_object_close(struct drm_gem_object *obj,
				     struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(obj->dev);
	struct nebulae_bo *bo = to_nebulae_bo(obj);
	struct nebulae_file *nfile = file->driver_priv;
	u64 va;
	int ret;

	if (!nfile || nebulae_vm_bo_va(nfile, bo, &va))
		return;

	/* GEM close runs before driver postclose and must never turn process exit
	 * into an uninterruptible fence wait.  Each committed job pins its VMAs;
	 * unmap is immediate when idle or deferred to the last job otherwise. */
	ret = nebulae_vm_unmap_bo(ndev, nfile, bo, true);
	if (ret && ret != -ENOENT && ret != -EINPROGRESS)
		drm_warn(obj->dev, "failed to release BO GPUVA on close: %d\n",
			 ret);
}

static const struct drm_gem_object_funcs nebulae_gem_object_funcs = {
	.free = nebulae_gem_object_free,
	.close = nebulae_gem_object_close,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	/* shmem is the sole CPU-authoritative backing for every placement.  VRAM
	 * is a device residency copy, never a second userspace-visible backing. */
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static int nebulae_bo_create_handle(struct drm_device *drm,
				    struct drm_file *file,
				    u64 requested_size, u32 flags,
				    u32 *handle, u64 *va)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_gem_shmem_object *shmem;
	struct nebulae_bo *bo;
	u64 mapped_va;
	size_t size;
	int ret;

	if (!requested_size || (flags & ~DRM_NEBULAE_BO_FLAGS))
		return -EINVAL;
	if (requested_size > SIZE_MAX)
		return -EOVERFLOW;

	size = PAGE_ALIGN((size_t)requested_size);
	if (!size)
		return -EINVAL;

	shmem = drm_gem_shmem_create(drm, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	bo = to_nebulae_bo(&shmem->base);
	bo->flags = flags;
	bo->domain = DRM_NEBULAE_BO_DOMAIN_CPU;
	shmem->map_wc = flags & DRM_NEBULAE_BO_WC;

	ret = nebulae_alloc_bo_va(ndev, bo, size);
	if (ret)
		goto err_put;

	/* Map this BO into the creating client's own address space, so its waves
	 * can reach it and other clients (with different page tables) cannot. */
	ret = nebulae_vm_map_bo(ndev, file->driver_priv, bo, &mapped_va);
	if (ret)
		goto err_free_va;

	mutex_lock(&ndev->bo_lock);
	if (!bo->listed) {
		list_add_tail(&bo->link, &ndev->bo_list);
		bo->listed = true;
	}
	mutex_unlock(&ndev->bo_lock);

	ret = drm_gem_handle_create(file, &shmem->base, handle);
	if (ret) {
		nebulae_vm_unmap_bo(ndev, file->driver_priv, bo, false);
	} else if (va) {
		*va = mapped_va;
	}
	drm_gem_object_put(&shmem->base);
	return ret;

err_free_va:
	nebulae_free_bo_va(ndev, bo);
err_put:
	drm_gem_object_put(&shmem->base);
	return ret;
}

static int nebulae_bo_wait_resv(struct nebulae_bo *bo,
				enum dma_resv_usage usage)
{
	struct drm_gem_object *obj = &bo->base.base;
	long ret;

	ret = dma_resv_wait_timeout(obj->resv, usage, true, MAX_SCHEDULE_TIMEOUT);
	if (ret < 0)
		return ret;
	if (!ret)
		return -ETIMEDOUT;
	return 0;
}

static int nebulae_bo_copy_to_vram(struct nebulae_device *ndev,
				   struct nebulae_bo *bo, bool wait)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->vram_offset || bo->vram_offset >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->vram_offset)
		return -EINVAL;

	if (wait) {
		ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_WRITE);
		if (ret)
			return ret;
	}

	/* drm_gem_shmem_vmap/vunmap assert the reservation lock is held and
	 * mutate pages_use_count; without it they race the KMS shadow-blit and
	 * shrinker paths, freeing shmem->pages under us (NULL deref in
	 * drm_gem_put_pages). */
	ret = dma_resv_lock(obj->resv, NULL);
	if (ret)
		return ret;

	ret = drm_gem_shmem_vmap(&bo->base, &map);
	if (ret) {
		dma_resv_unlock(obj->resv);
		return ret;
	}
	if (map.is_iomem) {
		drm_gem_shmem_vunmap(&bo->base, &map);
		dma_resv_unlock(obj->resv);
		return -EINVAL;
	}

	memcpy_toio(ndev->vram + bo->vram_offset, map.vaddr, obj->size);
	bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_CPU;
	bo->domain |= DRM_NEBULAE_BO_DOMAIN_GPU;
	drm_gem_shmem_vunmap(&bo->base, &map);
	dma_resv_unlock(obj->resv);
	return 0;
}

int nebulae_bo_sync_to_vram(struct nebulae_device *ndev,
			    struct nebulae_bo *bo)
{
	return nebulae_bo_copy_to_vram(ndev, bo, true);
}

int nebulae_bo_sync_to_vram_nowait(struct nebulae_device *ndev,
				   struct nebulae_bo *bo)
{
	return nebulae_bo_copy_to_vram(ndev, bo, false);
}

static int nebulae_bo_copy_from_vram(struct nebulae_device *ndev,
				     struct nebulae_bo *bo, bool wait)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->vram_offset || bo->vram_offset >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->vram_offset)
		return -EINVAL;

	if (wait) {
		ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_READ);
		if (ret)
			return ret;
	}

	/* See nebulae_bo_sync_to_vram: the reservation lock is required around
	 * drm_gem_shmem_vmap/vunmap to avoid racing page teardown. */
	ret = dma_resv_lock(obj->resv, NULL);
	if (ret)
		return ret;

	ret = drm_gem_shmem_vmap(&bo->base, &map);
	if (ret) {
		dma_resv_unlock(obj->resv);
		return ret;
	}
	if (map.is_iomem) {
		drm_gem_shmem_vunmap(&bo->base, &map);
		dma_resv_unlock(obj->resv);
		return -EINVAL;
	}

	memcpy_fromio(map.vaddr, ndev->vram + bo->vram_offset, obj->size);
	bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_GPU;
	bo->domain |= DRM_NEBULAE_BO_DOMAIN_CPU;
	drm_gem_shmem_vunmap(&bo->base, &map);
	dma_resv_unlock(obj->resv);
	return 0;
}

int nebulae_bo_sync_from_vram(struct nebulae_device *ndev,
			      struct nebulae_bo *bo)
{
	return nebulae_bo_copy_from_vram(ndev, bo, true);
}

int nebulae_bo_sync_from_vram_nowait(struct nebulae_device *ndev,
				     struct nebulae_bo *bo)
{
	return nebulae_bo_copy_from_vram(ndev, bo, false);
}

int nebulae_ioctl_bo_create(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_nebulae_bo_create *args = data;
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_bo_create_handle(drm, file, args->size, args->flags,
				       &args->handle, &args->va);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

static int nebulae_dumb_create_active(struct drm_file *file,
				      struct drm_device *drm,
				      struct drm_mode_create_dumb *args)
{
	u64 min_pitch;
	u64 size;
	u32 flags;
	int ret;

	if (!args->width || !args->height || !args->bpp)
		return -EINVAL;

	min_pitch = DIV_ROUND_UP_ULL((u64)args->width * args->bpp, 8);
	min_pitch = ALIGN(min_pitch, 64);
	if (min_pitch > U32_MAX)
		return -EOVERFLOW;

	if (!args->pitch || args->pitch < min_pitch)
		args->pitch = min_pitch;

	size = (u64)args->pitch * args->height;
	if (args->height && size / args->height != args->pitch)
		return -EOVERFLOW;
	size = PAGE_ALIGN(size);
	if (!size)
		return -EINVAL;
	args->size = size;

	flags = DRM_NEBULAE_BO_WC |
		DRM_NEBULAE_BO_PLACEMENT_DEVICE_LOCAL |
		DRM_NEBULAE_BO_TYPE_RESOURCE;
	ret = nebulae_bo_create_handle(drm, file, args->size, flags,
				       &args->handle, NULL);
	if (!ret)
		drm_dbg_kms(drm,
			    "dumb_create: %ux%u bpp=%u pitch=%u size=%llu flags=0x%x\n",
			    args->width, args->height, args->bpp, args->pitch,
			    args->size, flags);
	return ret;
}

int nebulae_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args)
{
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_dumb_create_active(file, drm, args);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

int nebulae_ioctl_bo_mmap(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_bo_mmap_offset *args = data;

	if (args->pad)
		return -EINVAL;

	return drm_gem_dumb_map_offset(file, drm, args->handle, &args->offset);
}

/* Map (or unmap) a BO into the calling client's address space.  Needed for
 * PRIME-imported BOs, which have no owning file at import time and so are not
 * mapped by the create path; the importer VM_BINDs them before use. */
static int nebulae_ioctl_vm_bind_active(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;
	struct drm_nebulae_vm_bind *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	u64 va = 0;
	int ret = 0;

	if (args->reserved[0] || args->reserved[1])
		return -EINVAL;
	if (args->op != DRM_NEBULAE_VM_BIND_OP_MAP &&
	    args->op != DRM_NEBULAE_VM_BIND_OP_UNMAP)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	bo = to_nebulae_bo(obj);
	if (!bo->vram_offset) {
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	ret = nebulae_vm_bo_va(nfile, bo, &va);
	if (args->op == DRM_NEBULAE_VM_BIND_OP_UNMAP) {
		if (ret)
			goto out_put;
		ret = nebulae_vm_unmap_bo(ndev, nfile, bo, false);
	} else {
		/* MAP is idempotent for a handle already bound in this file. */
		if (ret == -ENOENT)
			ret = nebulae_vm_map_bo(ndev, nfile, bo, &va);
		else if (!ret)
			ret = 0;
	}
	args->va = va;

out_put:
	drm_gem_object_put(obj);
	return ret;
}

int nebulae_ioctl_vm_bind(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_ioctl_vm_bind_active(drm, data, file);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

int nebulae_ioctl_bo_wait(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_bo_wait *args = data;
	struct drm_gem_object *obj;
	unsigned long timeout = MAX_SCHEDULE_TIMEOUT;
	long ret;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if (args->timeout_ns) {
		u64 jiffies64 = nsecs_to_jiffies64(args->timeout_ns);

		timeout = min_t(u64, jiffies64 ?: 1, MAX_SCHEDULE_TIMEOUT);
	}
	ret = dma_resv_wait_timeout(obj->resv, DMA_RESV_USAGE_READ, true,
				    timeout);
	if (!ret)
		ret = -ETIMEDOUT;
	drm_gem_object_put(obj);
	return ret < 0 ? ret : 0;
}

static int nebulae_ioctl_madvise_active(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct drm_nebulae_madvise *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	int ret;

	if (args->pad || (args->madv != DRM_NEBULAE_MADV_WILLNEED &&
			  args->madv != DRM_NEBULAE_MADV_DONTNEED))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	bo = to_nebulae_bo(obj);
	/* An imported object has an external backing lifetime; pretending it can
	 * be purged would corrupt aliases. */
	if (obj->import_attach || obj->dma_buf) {
		ret = -EOPNOTSUPP;
		goto out_put;
	}

	ret = dma_resv_lock_interruptible(obj->resv, NULL);
	if (ret)
		goto out_put;

	args->retained = drm_gem_shmem_madvise(&bo->base, args->madv);
	if (args->madv == DRM_NEBULAE_MADV_DONTNEED &&
	    drm_gem_shmem_is_purgeable(&bo->base)) {
		int unmap_ret;

		/* Remove the caller's GPU mapping before discarding pages so the GPU
		 * can never observe stale VRAM after retained becomes false. */
		unmap_ret = nebulae_vm_unmap_bo(to_nebulae(drm),
						 file->driver_priv, bo, false);
		if (!unmap_ret || unmap_ret == -ENOENT) {
			drm_gem_shmem_purge(&bo->base);
			args->retained = 0;
		} else if (unmap_ret == -EBUSY) {
			/* An in-flight job still owns the pages.  Retain both the object and
			 * its usable mapping; userspace may retry after retirement. */
			args->retained = 1;
		} else {
			ret = unmap_ret;
			goto out_unlock;
		}
	}
	dma_resv_unlock(obj->resv);
	ret = 0;
	goto out_put;

out_unlock:
	dma_resv_unlock(obj->resv);

out_put:
	drm_gem_object_put(obj);
	return ret;
}

int nebulae_ioctl_madvise(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_ioctl_madvise_active(drm, data, file);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

int nebulae_ioctl_bo_info(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_bo_info *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	u64 va;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	bo = to_nebulae_bo(obj);
	ret = nebulae_vm_bo_va(file->driver_priv, bo, &va);
	if (ret) {
		drm_gem_object_put(obj);
		return ret;
	}
	args->flags = bo->flags;
	args->size = obj->size;
	args->va = va;
	args->placement = bo->flags & DRM_NEBULAE_BO_PLACEMENT_MASK;
	args->domain = bo->domain;
	drm_gem_object_put(obj);
	return 0;
}

static bool nebulae_bo_write_domain_valid(u32 domain)
{
	return !domain || domain == DRM_NEBULAE_BO_DOMAIN_CPU ||
	       domain == DRM_NEBULAE_BO_DOMAIN_GPU;
}

static int nebulae_ioctl_bo_set_domain_active(struct drm_device *drm,
					      void *data,
					      struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_bo_set_domain *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	int ret = 0;

	if (args->pad ||
	    (args->read_domains & ~DRM_NEBULAE_BO_DOMAIN_MASK) ||
	    (args->write_domain & ~DRM_NEBULAE_BO_DOMAIN_MASK) ||
	    !nebulae_bo_write_domain_valid(args->write_domain))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	bo = to_nebulae_bo(obj);

	if (args->read_domains & DRM_NEBULAE_BO_DOMAIN_GPU) {
		if (bo->domain & DRM_NEBULAE_BO_DOMAIN_CPU)
			ret = nebulae_bo_sync_to_vram(ndev, bo);
		else
			ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_WRITE);
	}
	if (!ret && (args->read_domains & DRM_NEBULAE_BO_DOMAIN_CPU) &&
	    (bo->domain & DRM_NEBULAE_BO_DOMAIN_GPU))
		ret = nebulae_bo_sync_from_vram(ndev, bo);
	if (!ret && args->write_domain) {
		if (args->write_domain == DRM_NEBULAE_BO_DOMAIN_CPU) {
			ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_READ);
			if (!ret) {
				bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_GPU;
				bo->domain |= DRM_NEBULAE_BO_DOMAIN_CPU;
			}
		} else {
			ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_WRITE);
			if (!ret) {
				bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_CPU;
				bo->domain |= DRM_NEBULAE_BO_DOMAIN_GPU;
			}
		}
	}

	drm_gem_object_put(obj);
	return ret;
}

int nebulae_ioctl_bo_set_domain(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_ioctl_bo_set_domain_active(drm, data, file);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

/* PRIME import (DRI3 client->server buffer sharing).  A same-device self-import
 * returns the original GEM object, preserving its VA and coherency domain.  A
 * cross-device / fresh import lands here without a VA, so bind one, mark it CPU
 * owned, and list it so the submit path syncs its shmem pages into VRAM before
 * the GPU samples it. */
struct drm_gem_object *nebulae_gem_prime_import(struct drm_device *drm,
						struct dma_buf *dma_buf)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	int ret;

	obj = drm_gem_prime_import(drm, dma_buf);
	if (IS_ERR(obj))
		return obj;

	bo = to_nebulae_bo(obj);
	if (!bo->vram_offset) {
		ret = nebulae_alloc_bo_va(ndev, bo, obj->size);
		if (ret) {
			drm_gem_object_put(obj);
			return ERR_PTR(ret);
		}
		bo->domain = DRM_NEBULAE_BO_DOMAIN_CPU;
	}

	mutex_lock(&ndev->bo_lock);
	if (!bo->listed) {
		list_add_tail(&bo->link, &ndev->bo_list);
		bo->listed = true;
	}
	mutex_unlock(&ndev->bo_lock);

	return obj;
}
