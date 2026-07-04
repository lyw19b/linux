// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae GEM/BO management.
 */

#include <linux/dma-buf.h>
#include <linux/dma-resv.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_prime.h>
#include <drm/drm_vma_manager.h>

#include "nebulae_internal.h"

static const struct drm_gem_object_funcs nebulae_gem_object_funcs;

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

/* BOs created with the DEVICE_LOCAL placement are resident in the VRAM window;
 * their CPU mmap maps that window directly (write-combine) so guest writes land
 * in VRAM (== the simulator RAM after Tier-0 aliasing) with no shmem->VRAM
 * staging copy. */
static bool nebulae_bo_vram_mapped(const struct nebulae_bo *bo)
{
	return (bo->flags & DRM_NEBULAE_BO_PLACEMENT_MASK) ==
	       DRM_NEBULAE_BO_PLACEMENT_DEVICE_LOCAL;
}

static const struct vm_operations_struct nebulae_gem_vram_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

/* Map a VRAM-resident BO's window straight into userspace, modelled on
 * drivers/gpu/drm/virtio/virtgpu_vram.c.  bo->va is page-aligned (the VA
 * allocator hands out PAGE_ALIGN'd nodes from a page-aligned base). */
static int nebulae_gem_mmap(struct drm_gem_object *obj,
			    struct vm_area_struct *vma)
{
	struct nebulae_device *ndev = to_nebulae(obj->dev);
	struct nebulae_bo *bo = to_nebulae_bo(obj);
	unsigned long vm_size = vma->vm_end - vma->vm_start;
	phys_addr_t phys;

	if (!nebulae_bo_vram_mapped(bo))
		return drm_gem_shmem_object_mmap(obj, vma);

	if (!bo->va || (bo->va & ~PAGE_MASK))
		return -EINVAL;

	vma->vm_pgoff -= drm_vma_node_start(&obj->vma_node);
	if (((u64)vma->vm_pgoff << PAGE_SHIFT) + vm_size > obj->size)
		return -EINVAL;
	phys = ndev->vram_phys + bo->va +
	       ((phys_addr_t)vma->vm_pgoff << PAGE_SHIFT);

	vm_flags_set(vma, VM_IO | VM_PFNMAP | VM_DONTEXPAND | VM_DONTDUMP);
	vma->vm_page_prot = pgprot_writecombine(vm_get_page_prot(vma->vm_flags));
	vma->vm_ops = &nebulae_gem_vram_vm_ops;

	return io_remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
				  vm_size, vma->vm_page_prot);
}

static const struct drm_gem_object_funcs nebulae_gem_object_funcs = {
	.free = nebulae_gem_object_free,
	.print_info = drm_gem_shmem_object_print_info,
	.pin = drm_gem_shmem_object_pin,
	.unpin = drm_gem_shmem_object_unpin,
	.get_sg_table = drm_gem_shmem_object_get_sg_table,
	.vmap = drm_gem_shmem_object_vmap,
	.vunmap = drm_gem_shmem_object_vunmap,
	.mmap = nebulae_gem_mmap,
	.vm_ops = &drm_gem_shmem_vm_ops,
};

static bool nebulae_bo_gpu_resident(const struct nebulae_bo *bo)
{
	return (bo->flags & DRM_NEBULAE_BO_PLACEMENT_MASK) ==
		       DRM_NEBULAE_BO_PLACEMENT_SHARED ||
	       (bo->flags & DRM_NEBULAE_BO_PLACEMENT_MASK) ==
		       DRM_NEBULAE_BO_PLACEMENT_DEVICE_LOCAL ||
	       (bo->domain & DRM_NEBULAE_BO_DOMAIN_SCANOUT);
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

static int nebulae_bo_sync_to_vram(struct nebulae_device *ndev,
				   struct nebulae_bo *bo)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->va || bo->va >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->va)
		return -EINVAL;

	ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_WRITE);
	if (ret)
		return ret;

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

	memcpy_toio(ndev->vram + bo->va, map.vaddr, obj->size);
	bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_CPU;
	bo->domain |= DRM_NEBULAE_BO_DOMAIN_GPU;
	drm_gem_shmem_vunmap(&bo->base, &map);
	dma_resv_unlock(obj->resv);
	return 0;
}

int nebulae_bo_sync_from_vram(struct nebulae_device *ndev,
			      struct nebulae_bo *bo)
{
	struct drm_gem_object *obj = &bo->base.base;
	struct iosys_map map;
	int ret;

	if (!bo->va || bo->va >= ndev->vram_size ||
	    obj->size > ndev->vram_size - bo->va)
		return -EINVAL;

	ret = nebulae_bo_wait_resv(bo, DMA_RESV_USAGE_READ);
	if (ret)
		return ret;

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

	memcpy_fromio(map.vaddr, ndev->vram + bo->va, obj->size);
	bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_GPU;
	bo->domain |= DRM_NEBULAE_BO_DOMAIN_CPU;
	drm_gem_shmem_vunmap(&bo->base, &map);
	dma_resv_unlock(obj->resv);
	return 0;
}

static bool nebulae_bo_should_sync_to_vram(struct nebulae_bo *bo)
{
	if (!bo->listed || !bo->va)
		return false;

	/* VRAM-mapped BOs are written by the CPU straight into VRAM; their shmem
	 * is unused, so staging it in would clobber the real contents. */
	if (nebulae_bo_vram_mapped(bo))
		return false;

	return !nebulae_bo_gpu_resident(bo) ||
	       (bo->domain & DRM_NEBULAE_BO_DOMAIN_CPU);
}

static bool nebulae_bo_should_sync_from_vram(struct nebulae_bo *bo)
{
	if (!bo->listed || !bo->va)
		return false;

	if (nebulae_bo_vram_mapped(bo))
		return false;

	return !nebulae_bo_gpu_resident(bo);
}

int nebulae_sync_all_bos_to_vram(struct nebulae_device *ndev)
{
	struct nebulae_bo *bo;
	int ret = 0;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		if (!nebulae_bo_should_sync_to_vram(bo))
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
		if (!bo->listed || !bo->va)
			continue;
		if (!nebulae_bo_should_sync_from_vram(bo)) {
			bo->domain &= ~DRM_NEBULAE_BO_DOMAIN_CPU;
			bo->domain |= DRM_NEBULAE_BO_DOMAIN_GPU;
			continue;
		}

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
	bo->domain = DRM_NEBULAE_BO_DOMAIN_CPU;
	shmem->map_wc = args->flags & DRM_NEBULAE_BO_WC;

	ret = nebulae_alloc_bo_va(ndev, bo, size);
	if (ret) {
		drm_gem_object_put(&shmem->base);
		return ret;
	}

	/* Map this BO into the creating client's own address space, so its waves
	 * can reach it and other clients (with different page tables) cannot. */
	ret = nebulae_mmu_map(ndev, file->driver_priv, bo->va, size);
	if (ret) {
		nebulae_free_bo_va(ndev, bo);
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

/* Map (or unmap) a BO into the calling client's address space.  Needed for
 * PRIME-imported BOs, which have no owning file at import time and so are not
 * mapped by the create path; the importer VM_BINDs them before use. */
int nebulae_ioctl_vm_bind(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;
	struct drm_nebulae_vm_bind *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	int ret = 0;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;
	bo = to_nebulae_bo(obj);
	if (!bo->va) {
		drm_gem_object_put(obj);
		return -EINVAL;
	}

	if (args->op == DRM_NEBULAE_VM_BIND_OP_UNMAP)
		nebulae_mmu_unmap(ndev, nfile, bo->va, obj->size);
	else
		ret = nebulae_mmu_map(ndev, nfile, bo->va, obj->size);

	/* Diagnostic: confirm the VM_BIND prime path maps imported BOs into the
	 * per-ASID page table.  info-level so it reaches console=ttyS0. */
	drm_info(drm,
		 "VM_BIND: op=%s handle=%u va=0x%llx size=%zu asid=%u imported=%d ret=%d\n",
		 args->op == DRM_NEBULAE_VM_BIND_OP_UNMAP ? "unmap" : "map",
		 args->handle, bo->va, obj->size, nfile->asid,
		 obj->import_attach != NULL, ret);

	drm_gem_object_put(obj);
	return ret;
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

int nebulae_ioctl_bo_info(struct drm_device *drm, void *data,
			  struct drm_file *file)
{
	struct drm_nebulae_bo_info *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	bo = to_nebulae_bo(obj);
	args->flags = bo->flags;
	args->size = obj->size;
	args->va = bo->va;
	args->placement = bo->flags & DRM_NEBULAE_BO_PLACEMENT_MASK;
	args->domain = bo->domain;
	drm_gem_object_put(obj);
	return bo->va ? 0 : -EINVAL;
}

static bool nebulae_bo_write_domain_valid(u32 domain)
{
	return !domain || domain == DRM_NEBULAE_BO_DOMAIN_CPU ||
	       domain == DRM_NEBULAE_BO_DOMAIN_GPU;
}

int nebulae_ioctl_bo_set_domain(struct drm_device *drm, void *data,
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

	mutex_lock(&ndev->bo_lock);
	if ((args->read_domains & DRM_NEBULAE_BO_DOMAIN_GPU) &&
	    (bo->domain & DRM_NEBULAE_BO_DOMAIN_CPU))
		ret = nebulae_bo_sync_to_vram(ndev, bo);
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
	mutex_unlock(&ndev->bo_lock);

	drm_gem_object_put(obj);
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
	if (!bo->va) {
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
