// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae GEM/BO management.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/slab.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>

#include "nebulae_internal.h"

static const struct drm_gem_object_funcs nebulae_gem_object_funcs;

struct drm_gem_object *nebulae_gem_create_object(struct drm_device *drm,
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

static const struct drm_gem_object_funcs nebulae_gem_object_funcs = {
	.free = nebulae_gem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = drm_gem_shmem_object_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static int nebulae_bo_sync_to_vram(struct nebulae_device *ndev,
				   struct nebulae_bo *bo)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->va || bo->va >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->va)
		return -EINVAL;

	ret = drm_gem_shmem_vmap(&bo->base, &map);
	if (ret)
		return ret;
	if (map.is_iomem) {
		drm_gem_shmem_vunmap(&bo->base, &map);
		return -EINVAL;
	}

	memcpy_toio(ndev->vram + bo->va, map.vaddr, obj->size);
	drm_gem_shmem_vunmap(&bo->base, &map);
	return 0;
}

static int nebulae_bo_sync_from_vram(struct nebulae_device *ndev,
				     struct nebulae_bo *bo)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->va || bo->va >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->va)
		return -EINVAL;

	ret = drm_gem_shmem_vmap(&bo->base, &map);
	if (ret)
		return ret;
	if (map.is_iomem) {
		drm_gem_shmem_vunmap(&bo->base, &map);
		return -EINVAL;
	}

	memcpy_fromio(map.vaddr, ndev->vram + bo->va, obj->size);
	drm_gem_shmem_vunmap(&bo->base, &map);
	return 0;
}

static bool nebulae_bo_should_auto_sync(struct nebulae_bo *bo)
{
	return bo->listed && bo->va &&
	       !(bo->flags & DRM_NEBULAE_BO_NO_AUTO_BIND);
}

int nebulae_sync_all_bos_to_vram(struct nebulae_device *ndev)
{
	struct nebulae_bo *bo;
	int ret = 0;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		if (!nebulae_bo_should_auto_sync(bo))
			continue;

		ret = nebulae_bo_sync_to_vram(ndev, bo);
		if (ret)
			break;
	}
	mutex_unlock(&ndev->bo_lock);

	return ret;
}

int nebulae_sync_all_bos_from_vram(struct nebulae_device *ndev)
{
	struct nebulae_bo *bo;
	int ret = 0;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		if (!nebulae_bo_should_auto_sync(bo))
			continue;

		ret = nebulae_bo_sync_from_vram(ndev, bo);
		if (ret)
			break;
	}
	mutex_unlock(&ndev->bo_lock);

	return ret;
}

int nebulae_ioctl_bo_create(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_bo_create *args = data;
	struct drm_gem_shmem_object *shmem;
	struct nebulae_bo *bo;
	size_t size;
	int ret;

	if (!args->size || (args->flags & ~DRM_NEBULAE_BO_FLAGS))
		return -EINVAL;

	if (args->size > SIZE_MAX)
		return -EOVERFLOW;

	size = PAGE_ALIGN((size_t)args->size);
	if (!size)
		return -EINVAL;

	shmem = drm_gem_shmem_create(drm, size);
	if (IS_ERR(shmem))
		return PTR_ERR(shmem);

	bo = to_nebulae_bo(&shmem->base);
	bo->flags = args->flags;
	shmem->map_wc = args->flags & DRM_NEBULAE_BO_WC;

	ret = nebulae_alloc_bo_va(ndev, bo, size);
	if (ret) {
		drm_gem_object_put(&shmem->base);
		return ret;
	}

	mutex_lock(&ndev->bo_lock);
	if (!bo->listed) {
		list_add_tail(&bo->link, &ndev->bo_list);
		bo->listed = true;
	}
	mutex_unlock(&ndev->bo_lock);

	ret = drm_gem_handle_create(file, &shmem->base, &args->handle);
	if (!ret)
		args->va = bo->va;
	drm_gem_object_put(&shmem->base);
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

int nebulae_ioctl_bo_wait(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_bo_wait *args = data;
	struct drm_gem_object *obj;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	drm_gem_object_put(obj);
	return 0;
}

int nebulae_ioctl_madvise(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_madvise *args = data;
	struct drm_gem_object *obj;

	if (args->pad)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	args->retained = 1;
	drm_gem_object_put(obj);
	return 0;
}
