// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae VRAM storage and per-file GPU virtual address management.
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/slab.h>

#include <drm/drm_drv.h>
#include <drm/drm_gem.h>

#include "nebulae_internal.h"

static u32 nebulae_bo_vm_prot(const struct nebulae_bo *bo)
{
	switch (bo->flags & DRM_NEBULAE_BO_TYPE_MASK) {
	case DRM_NEBULAE_BO_TYPE_COMMAND:
		return NEB_VM_PROT_READ | NEB_VM_PROT_USER;
	case DRM_NEBULAE_BO_TYPE_SHADER:
		return NEB_VM_PROT_READ | NEB_VM_PROT_EXEC | NEB_VM_PROT_USER;
	case DRM_NEBULAE_BO_TYPE_RESOURCE:
	case 0: /* Legacy BOs are data, never executable. */
	default:
		return NEB_VM_PROT_READ | NEB_VM_PROT_WRITE |
		       NEB_VM_PROT_USER;
	}
}

int nebulae_vm_init(struct nebulae_device *ndev)
{
	if (ndev->vram_size <= NEB_SCANOUT_RESERVED + NEB_GPU_PAGE_SIZE)
		return -EINVAL;

	ndev->vm_start = NEB_GPU_VA_START;
	ndev->vm_size = NEB_GPU_VA_SIZE;
	drm_mm_init(&ndev->vram_mm, NEB_SCANOUT_RESERVED,
		    ndev->vram_size - NEB_SCANOUT_RESERVED);
	return 0;
}

void nebulae_vm_fini(struct nebulae_device *ndev)
{
	if (!drm_mm_clean(&ndev->vram_mm))
		drm_warn(&ndev->drm, "tearing down non-empty VRAM space\n");

	drm_mm_takedown(&ndev->vram_mm);
}

int nebulae_vm_file_init(struct nebulae_file *nfile)
{
	mutex_init(&nfile->vm_lock);
	INIT_LIST_HEAD(&nfile->vmas);
	drm_mm_init(&nfile->va_mm, NEB_GPU_VA_START, NEB_GPU_VA_SIZE);
	return 0;
}

int nebulae_alloc_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo,
			u64 size)
{
	u64 alloc_size;
	int ret;

	if (check_add_overflow(size, NEB_GPU_PAGE_SIZE - 1, &alloc_size))
		return -EOVERFLOW;
	alloc_size = ALIGN(size, NEB_GPU_PAGE_SIZE);

	mutex_lock(&ndev->bo_lock);
	ret = drm_mm_insert_node_generic(&ndev->vram_mm, &bo->vram_node,
					 alloc_size, NEB_GPU_PAGE_SIZE, 0, 0);
	if (!ret)
		bo->vram_offset = bo->vram_node.start;
	mutex_unlock(&ndev->bo_lock);

	return ret;
}

void nebulae_free_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo)
{
	mutex_lock(&ndev->bo_lock);
	if (drm_mm_node_allocated(&bo->vram_node)) {
		drm_mm_remove_node(&bo->vram_node);
		bo->vram_offset = 0;
	}
	mutex_unlock(&ndev->bo_lock);
}

static struct nebulae_vma *
nebulae_vm_find_bo_locked(struct nebulae_file *nfile, struct nebulae_bo *bo)
{
	struct nebulae_vma *vma;

	list_for_each_entry(vma, &nfile->vmas, vm_link) {
		if (vma->bo == bo)
			return vma;
	}
	return NULL;
}

static int nebulae_vm_map(struct nebulae_device *ndev,
			  struct nebulae_file *nfile, struct nebulae_bo *bo,
			  u64 phys, u64 size, u32 prot,
			  struct nebulae_vma **out_vma)
{
	struct nebulae_vma *vma;
	u64 map_size;
	int ret;

	if (!size || !IS_ALIGNED(phys, NEB_GPU_PAGE_SIZE))
		return -EINVAL;
	if (check_add_overflow(size, NEB_GPU_PAGE_SIZE - 1, &map_size))
		return -EOVERFLOW;
	map_size = ALIGN(size, NEB_GPU_PAGE_SIZE);

	vma = kzalloc(sizeof(*vma), GFP_KERNEL);
	if (!vma)
		return -ENOMEM;

	vma->nfile = nfile;
	vma->bo = bo;
	vma->phys = phys;
	vma->prot = prot;
	INIT_LIST_HEAD(&vma->vm_link);

	mutex_lock(&nfile->vm_lock);
	if (bo && nebulae_vm_find_bo_locked(nfile, bo)) {
		ret = -EEXIST;
		goto err_unlock;
	}

	ret = drm_mm_insert_node_generic(&nfile->va_mm, &vma->node, map_size,
						 NEB_GPU_PAGE_SIZE, 0, 0);
	if (ret)
		goto err_unlock;

	ret = nebulae_mmu_map(ndev, nfile, vma->node.start, phys, map_size,
			      prot);
	if (ret) {
		drm_mm_remove_node(&vma->node);
		goto err_unlock;
	}

	if (bo)
		drm_gem_object_get(&bo->base.base);
	list_add_tail(&vma->vm_link, &nfile->vmas);
	mutex_unlock(&nfile->vm_lock);
	*out_vma = vma;
	return 0;

err_unlock:
	mutex_unlock(&nfile->vm_lock);
	kfree(vma);
	return ret;
}

int nebulae_vm_map_bo(struct nebulae_device *ndev,
		      struct nebulae_file *nfile, struct nebulae_bo *bo,
		      u64 *va)
{
	struct nebulae_vma *vma;
	int ret;

	ret = nebulae_vm_map(ndev, nfile, bo, bo->vram_offset,
			      bo->base.base.size, nebulae_bo_vm_prot(bo), &vma);
	if (!ret && va)
		*va = vma->node.start;
	return ret;
}

int nebulae_vm_map_kernel(struct nebulae_device *ndev,
			  struct nebulae_file *nfile, u64 phys, u64 size,
			  u32 prot, struct nebulae_vma **out_vma)
{
	return nebulae_vm_map(ndev, nfile, NULL, phys, size, prot, out_vma);
}

static void nebulae_vm_remove_locked(struct nebulae_device *ndev,
				     struct nebulae_vma *vma)
{
	if (!READ_ONCE(ndev->unplugged))
		nebulae_mmu_unmap(ndev, vma->nfile, vma->node.start,
				  vma->node.size);
	drm_mm_remove_node(&vma->node);
	list_del_init(&vma->vm_link);
}

int nebulae_vm_unmap_bo(struct nebulae_device *ndev,
			struct nebulae_file *nfile, struct nebulae_bo *bo,
			bool defer_busy)
{
	struct nebulae_vma *vma;

	mutex_lock(&nfile->vm_lock);
	vma = nebulae_vm_find_bo_locked(nfile, bo);
	if (!vma) {
		mutex_unlock(&nfile->vm_lock);
		return -ENOENT;
	}
	/* No unmap path waits for a GPU fence.  File/GEM teardown may transfer
	 * ownership to the last committed job; explicit VM operations must instead
	 * report busy without changing the mapping, so a successful ioctl never
	 * names a VA that is pending asynchronous destruction. */
	if (vma->job_refs) {
		if (!defer_busy) {
			mutex_unlock(&nfile->vm_lock);
			return -EBUSY;
		}
		vma->pending_unmap = true;
		mutex_unlock(&nfile->vm_lock);
		return -EINPROGRESS;
	}
	nebulae_vm_remove_locked(ndev, vma);
	mutex_unlock(&nfile->vm_lock);
	drm_gem_object_put(&bo->base.base);
	kfree(vma);
	return 0;
}

int nebulae_vm_job_pin_bos(struct nebulae_file *nfile,
			   struct drm_gem_object **objs, u32 obj_count,
			   struct nebulae_vma ***vmas, u32 *vma_count)
{
	struct nebulae_vma **pins;
	u32 i;
	int ret = 0;

	*vmas = NULL;
	*vma_count = 0;
	if (!obj_count)
		return 0;

	pins = kvmalloc_array(obj_count, sizeof(*pins), GFP_KERNEL);
	if (!pins)
		return -ENOMEM;

	mutex_lock(&nfile->vm_lock);
	for (i = 0; i < obj_count; i++) {
		struct nebulae_vma *vma =
			nebulae_vm_find_bo_locked(nfile, to_nebulae_bo(objs[i]));

		if (!vma || vma->pending_unmap) {
			ret = -ENOENT;
			break;
		}
		vma->job_refs++;
		pins[i] = vma;
	}
	if (ret) {
		while (i)
			pins[--i]->job_refs--;
	}
	mutex_unlock(&nfile->vm_lock);

	if (ret) {
		kvfree(pins);
		return ret;
	}
	*vmas = pins;
	*vma_count = obj_count;
	return 0;
}

void nebulae_vm_job_unpin_bos(struct nebulae_device *ndev,
			      struct nebulae_file *nfile,
			      struct nebulae_vma ***vmas, u32 *vma_count)
{
	struct nebulae_vma **pins = *vmas;
	LIST_HEAD(reap);
	u32 i;

	if (!pins)
		return;

	mutex_lock(&nfile->vm_lock);
	for (i = 0; i < *vma_count; i++) {
		struct nebulae_vma *vma = pins[i];

		if (WARN_ON(!vma->job_refs))
			continue;
		vma->job_refs--;
		if (!vma->job_refs && vma->pending_unmap) {
			nebulae_vm_remove_locked(ndev, vma);
			list_add_tail(&vma->vm_link, &reap);
		}
	}
	mutex_unlock(&nfile->vm_lock);

	while (!list_empty(&reap)) {
		struct nebulae_vma *vma =
			list_first_entry(&reap, struct nebulae_vma, vm_link);

		list_del(&vma->vm_link);
		if (vma->bo)
			drm_gem_object_put(&vma->bo->base.base);
		kfree(vma);
	}
	kvfree(pins);
	*vmas = NULL;
	*vma_count = 0;
}

void nebulae_vm_unmap_kernel(struct nebulae_device *ndev,
			     struct nebulae_file *nfile,
			     struct nebulae_vma *vma)
{
	if (!vma)
		return;
	mutex_lock(&nfile->vm_lock);
	nebulae_vm_remove_locked(ndev, vma);
	mutex_unlock(&nfile->vm_lock);
	kfree(vma);
}

int nebulae_vm_bo_va(struct nebulae_file *nfile, struct nebulae_bo *bo,
		     u64 *va)
{
	struct nebulae_vma *vma;
	int ret = 0;

	mutex_lock(&nfile->vm_lock);
	vma = nebulae_vm_find_bo_locked(nfile, bo);
	if (!vma)
		ret = -ENOENT;
	else
		*va = vma->node.start;
	mutex_unlock(&nfile->vm_lock);
	return ret;
}

bool nebulae_vm_range_valid(struct nebulae_file *nfile, u64 va, u64 size,
			    u32 prot, struct nebulae_bo **bo_out)
{
	struct nebulae_vma *vma;
	bool valid = false;
	u64 end;

	if (!size || check_add_overflow(va, size, &end))
		return false;

	mutex_lock(&nfile->vm_lock);
	list_for_each_entry(vma, &nfile->vmas, vm_link) {
		u64 vma_end = vma->node.start + vma->node.size;

		if (va < vma->node.start || end > vma_end)
			continue;
		if ((vma->prot & prot) != prot)
			continue;
		if (bo_out)
			*bo_out = vma->bo;
		valid = true;
		break;
	}
	mutex_unlock(&nfile->vm_lock);
	return valid;
}

int nebulae_vm_restore(struct nebulae_device *ndev,
		       struct nebulae_file *nfile)
{
	struct nebulae_vma *vma;
	int ret;

	ret = nebulae_mmu_ctx_restore(ndev, nfile);
	if (ret)
		return ret;

	mutex_lock(&nfile->vm_lock);
	list_for_each_entry(vma, &nfile->vmas, vm_link) {
		ret = nebulae_mmu_map(ndev, nfile, vma->node.start, vma->phys,
				      vma->node.size, vma->prot);
		if (ret)
			break;
	}
	mutex_unlock(&nfile->vm_lock);
	return ret;
}

void nebulae_vm_file_fini(struct nebulae_device *ndev,
			  struct nebulae_file *nfile)
{
	struct nebulae_vma *vma, *tmp;
	LIST_HEAD(put_list);

	mutex_lock(&nfile->vm_lock);
	list_for_each_entry_safe(vma, tmp, &nfile->vmas, vm_link) {
		WARN_ON(vma->job_refs);
		nebulae_vm_remove_locked(ndev, vma);
		list_add_tail(&vma->vm_link, &put_list);
	}
	mutex_unlock(&nfile->vm_lock);

	list_for_each_entry_safe(vma, tmp, &put_list, vm_link) {
		list_del(&vma->vm_link);
		if (vma->bo)
			drm_gem_object_put(&vma->bo->base.base);
		kfree(vma);
	}

	WARN_ON(!drm_mm_clean(&nfile->va_mm));
	drm_mm_takedown(&nfile->va_mm);
}
