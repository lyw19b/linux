// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae virtual address management.
 */

#include <linux/errno.h>
#include <linux/mm.h>

#include "nebulae_internal.h"

int nebulae_vm_init(struct nebulae_device *ndev)
{
	if (ndev->vram_size <= NEB_SCANOUT_RESERVED + PAGE_SIZE)
		return -EINVAL;

	ndev->vm_start = NEB_SCANOUT_RESERVED;
	ndev->vm_size = ndev->vram_size - ndev->vm_start;
	ndev->next_va = ndev->vm_start;
	return 0;
}

void nebulae_vm_fini(struct nebulae_device *ndev)
{
}

int nebulae_alloc_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo,
			u64 size)
{
	u64 va;
	u64 end;
	int ret = 0;

	mutex_lock(&ndev->bo_lock);
	va = ALIGN(ndev->next_va, PAGE_SIZE);
	end = va + size;
	if (end < va || end > ndev->vram_size) {
		ret = -ENOSPC;
	} else {
		bo->va = va;
		ndev->next_va = end;
	}
	mutex_unlock(&ndev->bo_lock);

	return ret;
}
