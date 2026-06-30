// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae virtual address management.
 */

#include <linux/errno.h>
#include <linux/mm.h>

#include <drm/drm_drv.h>

#include "nebulae_internal.h"

int nebulae_vm_init(struct nebulae_device *ndev)
{
	if (ndev->vram_size <= NEB_SCANOUT_RESERVED + PAGE_SIZE)
		return -EINVAL;

	ndev->vm_start = NEB_SCANOUT_RESERVED;
	ndev->vm_size = ndev->vram_size - ndev->vm_start;
	ndev->next_va = ndev->vm_start;
	drm_mm_init(&ndev->va_mm, ndev->vm_start, ndev->vm_size);
	return 0;
}

void nebulae_vm_fini(struct nebulae_device *ndev)
{
	if (!drm_mm_clean(&ndev->va_mm))
		drm_warn(&ndev->drm, "tearing down non-empty VA space\n");

	drm_mm_takedown(&ndev->va_mm);
}

int nebulae_alloc_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo,
			u64 size)
{
	int ret;

	mutex_lock(&ndev->bo_lock);
	ret = drm_mm_insert_node(&ndev->va_mm, &bo->va_node,
				 PAGE_ALIGN(size));
	if (!ret)
		bo->va = bo->va_node.start;
	mutex_unlock(&ndev->bo_lock);

	return ret;
}

void nebulae_free_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo)
{
	mutex_lock(&ndev->bo_lock);
	if (drm_mm_node_allocated(&bo->va_node)) {
		drm_mm_remove_node(&bo->va_node);
		bo->va = 0;
	}
	mutex_unlock(&ndev->bo_lock);
}
