// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae 48-bit, three-level GPU MMU.
 *
 * The device page size is an ABI property (16 KiB), independent of the host
 * kernel PAGE_SIZE.  Each DRM file owns one ASID and one page-table slot.
 */

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/overflow.h>
#include <linux/sizes.h>

#include <drm/drm_print.h>

#include "nebulae_internal.h"

#define NEB_MMU_PAGE_SHIFT	14
#define NEB_MMU_PAGE_SIZE	(1ULL << NEB_MMU_PAGE_SHIFT)
#define NEB_MMU_PAGE_MASK	(NEB_MMU_PAGE_SIZE - 1)

#define NEB_MMU_L0_SHIFT	NEB_MMU_PAGE_SHIFT
#define NEB_MMU_L0_BITS		11
#define NEB_MMU_L1_SHIFT	25
#define NEB_MMU_L1_BITS		11
#define NEB_MMU_L2_SHIFT	36
#define NEB_MMU_L2_BITS		12

#define NEB_MMU_L0_ENTRIES	(1U << NEB_MMU_L0_BITS)
#define NEB_MMU_L1_ENTRIES	(1U << NEB_MMU_L1_BITS)
#define NEB_MMU_L2_ENTRIES	(1U << NEB_MMU_L2_BITS)

#define NEB_MMU_L0_SIZE		(NEB_MMU_L0_ENTRIES * sizeof(u64))
#define NEB_MMU_L1_SIZE		(NEB_MMU_L1_ENTRIES * sizeof(u64))
#define NEB_MMU_L2_SIZE		(NEB_MMU_L2_ENTRIES * sizeof(u64))

#define NEB_MMU_VA_MASK		((1ULL << NEB_GPU_VA_BITS) - 1)
#define NEB_MMU_PTE_ADDR_MASK	(~NEB_MMU_PAGE_MASK)
#define NEB_MMU_PTE_VALID	BIT_ULL(0)
#define NEB_MMU_PTE_READ	BIT_ULL(1)
#define NEB_MMU_PTE_WRITE	BIT_ULL(2)
#define NEB_MMU_PTE_EXEC	BIT_ULL(3)
#define NEB_MMU_PTE_USER	BIT_ULL(4)

#define NEB_MMU_CTX_PT_BYTES	SZ_512K
#define NEB_MMU_PT_PAGES	(NEB_MMU_CTX_PT_BYTES / NEB_MMU_PAGE_SIZE)
#define NEB_MMU_POOL_SIZE	(NEB_MMU_CTX_PT_BYTES * NEB_MMU_MAX_CTX)

static void neb_mmu_invalidate_tlbs(struct nebulae_device *ndev)
{
	/* The current register contract requests a global shootdown.  The device
	 * may coalesce requests and applies them at an ordered job boundary. */
	if (ndev->hw_caps & NEB_CAP_TLB_INVALIDATE) {
		wmb();
		writel(NEB_TLB_INVALIDATE_ALL,
		       ndev->regs + NEB_REG_TLB_INVALIDATE);
	}
}

static u64 neb_mmu_l2_index(u64 va)
{
	return (va >> NEB_MMU_L2_SHIFT) & (NEB_MMU_L2_ENTRIES - 1);
}

static u64 neb_mmu_l1_index(u64 va)
{
	return (va >> NEB_MMU_L1_SHIFT) & (NEB_MMU_L1_ENTRIES - 1);
}

static u64 neb_mmu_l0_index(u64 va)
{
	return (va >> NEB_MMU_L0_SHIFT) & (NEB_MMU_L0_ENTRIES - 1);
}

static u64 neb_mmu_read_pte(struct nebulae_device *ndev, u64 table,
			    u64 index)
{
	return readq(ndev->vram + table + index * sizeof(u64));
}

static void neb_mmu_write_pte(struct nebulae_device *ndev, u64 table,
			      u64 index, u64 pte)
{
	writeq(pte, ndev->vram + table + index * sizeof(u64));
}

static u64 neb_mmu_leaf_flags(u32 prot)
{
	u64 flags = NEB_MMU_PTE_VALID;

	if (prot & NEB_VM_PROT_READ)
		flags |= NEB_MMU_PTE_READ;
	if (prot & NEB_VM_PROT_WRITE)
		flags |= NEB_MMU_PTE_WRITE;
	if (prot & NEB_VM_PROT_EXEC)
		flags |= NEB_MMU_PTE_EXEC;
	if (prot & NEB_VM_PROT_USER)
		flags |= NEB_MMU_PTE_USER;
	return flags;
}

static int neb_mmu_slot_alloc_table(struct nebulae_device *ndev,
				    struct nebulae_file *nfile,
				    u64 size, u64 *out)
{
	unsigned long nr_pages = DIV_ROUND_UP_ULL(size, NEB_MMU_PAGE_SIZE);
	unsigned long page;

	page = bitmap_find_next_zero_area(nfile->mmu_pt_bitmap,
					 NEB_MMU_PT_PAGES, 0, nr_pages, 0);
	if (page >= NEB_MMU_PT_PAGES)
		return -ENOSPC;

	bitmap_set(nfile->mmu_pt_bitmap, page, nr_pages);
	*out = nfile->mmu_slot_base + page * NEB_MMU_PAGE_SIZE;
	memset_io(ndev->vram + *out, 0, nr_pages * NEB_MMU_PAGE_SIZE);
	return 0;
}

static void neb_mmu_slot_free_table(struct nebulae_file *nfile, u64 table,
				    u64 size)
{
	unsigned long page;
	unsigned long nr_pages = DIV_ROUND_UP_ULL(size, NEB_MMU_PAGE_SIZE);

	if (WARN_ON(table < nfile->mmu_slot_base ||
		    table >= nfile->mmu_slot_base + NEB_MMU_CTX_PT_BYTES))
		return;
	page = (table - nfile->mmu_slot_base) / NEB_MMU_PAGE_SIZE;
	bitmap_clear(nfile->mmu_pt_bitmap, page, nr_pages);
}

static bool neb_mmu_table_empty(struct nebulae_device *ndev, u64 table,
				unsigned int entries)
{
	unsigned int i;

	for (i = 0; i < entries; i++) {
		if (neb_mmu_read_pte(ndev, table, i) & NEB_MMU_PTE_VALID)
			return false;
	}
	return true;
}

static void neb_mmu_unmap_range_locked(struct nebulae_device *ndev,
				       struct nebulae_file *nfile,
				       u64 va, u64 size)
{
	u64 end;

	if (!size || check_add_overflow(va, size, &end))
		return;
	end = ALIGN(end, NEB_MMU_PAGE_SIZE);
	va = ALIGN_DOWN(va, NEB_MMU_PAGE_SIZE);

	for (; va < end; va += NEB_MMU_PAGE_SIZE) {
		u64 l2_idx = neb_mmu_l2_index(va);
		u64 l1_idx = neb_mmu_l1_index(va);
		u64 pte = neb_mmu_read_pte(ndev, nfile->mmu_root, l2_idx);
		u64 l1, l0;

		if (!(pte & NEB_MMU_PTE_VALID))
			continue;
		l1 = pte & NEB_MMU_PTE_ADDR_MASK;
		pte = neb_mmu_read_pte(ndev, l1, l1_idx);
		if (!(pte & NEB_MMU_PTE_VALID))
			continue;
		l0 = pte & NEB_MMU_PTE_ADDR_MASK;
		neb_mmu_write_pte(ndev, l0, neb_mmu_l0_index(va), 0);

		if (neb_mmu_table_empty(ndev, l0, NEB_MMU_L0_ENTRIES)) {
			neb_mmu_write_pte(ndev, l1, l1_idx, 0);
			neb_mmu_slot_free_table(nfile, l0, NEB_MMU_L0_SIZE);
			if (neb_mmu_table_empty(ndev, l1, NEB_MMU_L1_ENTRIES)) {
				neb_mmu_write_pte(ndev, nfile->mmu_root, l2_idx, 0);
				neb_mmu_slot_free_table(nfile, l1, NEB_MMU_L1_SIZE);
			}
		}
	}
}

static int neb_mmu_map_range_locked(struct nebulae_device *ndev,
				    struct nebulae_file *nfile,
				    u64 va, u64 phys, u64 size, u32 prot)
{
	u64 start = va;
	u64 end;
	u64 phys_end;
	u64 flags;
	int ret;

	if (!size || !prot || !IS_ALIGNED(va, NEB_MMU_PAGE_SIZE) ||
	    !IS_ALIGNED(phys, NEB_MMU_PAGE_SIZE) ||
	    !IS_ALIGNED(size, NEB_MMU_PAGE_SIZE))
		return -EINVAL;
	if (check_add_overflow(va, size, &end) || end - 1 > NEB_MMU_VA_MASK)
		return -EOVERFLOW;
	if (check_add_overflow(phys, size, &phys_end) || phys_end > ndev->vram_size)
		return -ERANGE;

	flags = neb_mmu_leaf_flags(prot);
	for (; va < end; va += NEB_MMU_PAGE_SIZE, phys += NEB_MMU_PAGE_SIZE) {
		u64 pte, l1, l0;

		pte = neb_mmu_read_pte(ndev, nfile->mmu_root,
				       neb_mmu_l2_index(va));
		if (pte & NEB_MMU_PTE_VALID) {
			l1 = pte & NEB_MMU_PTE_ADDR_MASK;
		} else {
			ret = neb_mmu_slot_alloc_table(ndev, nfile,
						       NEB_MMU_L1_SIZE, &l1);
			if (ret)
				goto rollback;
			neb_mmu_write_pte(ndev, nfile->mmu_root,
					  neb_mmu_l2_index(va),
					  l1 | NEB_MMU_PTE_VALID);
		}

		pte = neb_mmu_read_pte(ndev, l1, neb_mmu_l1_index(va));
		if (pte & NEB_MMU_PTE_VALID) {
			l0 = pte & NEB_MMU_PTE_ADDR_MASK;
		} else {
			ret = neb_mmu_slot_alloc_table(ndev, nfile,
						       NEB_MMU_L0_SIZE, &l0);
			if (ret)
				goto rollback;
			neb_mmu_write_pte(ndev, l1, neb_mmu_l1_index(va),
					  l0 | NEB_MMU_PTE_VALID);
		}

		/* Overlap is a VM programming error; never silently replace a PTE. */
		if (neb_mmu_read_pte(ndev, l0, neb_mmu_l0_index(va)) &
		    NEB_MMU_PTE_VALID) {
			ret = -EEXIST;
			goto rollback;
		}
		neb_mmu_write_pte(ndev, l0, neb_mmu_l0_index(va),
				  (phys & NEB_MMU_PTE_ADDR_MASK) | flags);
	}
	wmb();
	return 0;

rollback:
	neb_mmu_unmap_range_locked(ndev, nfile, start, va - start);
	wmb();
	return ret;
}

int nebulae_mmu_init(struct nebulae_device *ndev)
{
	u64 pool_base;
	int ret;

	mutex_init(&ndev->mmu_lock);
	bitmap_zero(ndev->mmu_ctx_bitmap, NEB_MMU_MAX_CTX);

	if (ndev->vram_size <= NEB_MMU_POOL_SIZE + NEB_SCANOUT_RESERVED)
		return -EINVAL;

	pool_base = ALIGN_DOWN(ndev->vram_size - NEB_MMU_POOL_SIZE,
			       NEB_MMU_PAGE_SIZE);
	ndev->mmu_pt_node.start = pool_base;
	ndev->mmu_pt_node.size = ndev->vram_size - pool_base;
	ret = drm_mm_reserve_node(&ndev->vram_mm, &ndev->mmu_pt_node);
	if (ret)
		return ret;

	ndev->mmu_pool_base = pool_base;
	ndev->mmu_pool_size = ndev->vram_size - pool_base;
	drm_dbg(&ndev->drm,
		"MMU: 48-bit VA, 3-level 16KiB pages, %d isolated ASIDs, PT pool [0x%llx,0x%llx)\n",
		NEB_MMU_MAX_CTX, ndev->mmu_pool_base,
		ndev->mmu_pool_base + ndev->mmu_pool_size);
	return 0;
}

void nebulae_mmu_fini(struct nebulae_device *ndev)
{
	if (drm_mm_node_allocated(&ndev->mmu_pt_node))
		drm_mm_remove_node(&ndev->mmu_pt_node);
}

static int nebulae_mmu_ctx_init_locked(struct nebulae_device *ndev,
				       struct nebulae_file *nfile)
{
	u64 root;
	int ret;

	bitmap_zero(nfile->mmu_pt_bitmap, NEB_MMU_PT_PAGES);
	memset_io(ndev->vram + nfile->mmu_slot_base, 0,
		  NEB_MMU_CTX_PT_BYTES);
	ret = neb_mmu_slot_alloc_table(ndev, nfile, NEB_MMU_L2_SIZE, &root);
	if (ret)
		return ret;
	nfile->mmu_root = root;
	return 0;
}

int nebulae_mmu_ctx_alloc(struct nebulae_device *ndev,
			  struct nebulae_file *nfile)
{
	int slot;
	int ret;

	mutex_lock(&ndev->mmu_lock);
	slot = find_first_zero_bit(ndev->mmu_ctx_bitmap, NEB_MMU_MAX_CTX);
	if (slot >= NEB_MMU_MAX_CTX) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	nfile->asid = (u32)slot + 1;
	nfile->mmu_slot_base = ndev->mmu_pool_base +
				  (u64)slot * NEB_MMU_CTX_PT_BYTES;
	ret = nebulae_mmu_ctx_init_locked(ndev, nfile);
	if (ret) {
		nfile->asid = 0;
		nfile->mmu_root = 0;
		goto out_unlock;
	}
	__set_bit(slot, ndev->mmu_ctx_bitmap);
	neb_mmu_invalidate_tlbs(ndev);

out_unlock:
	mutex_unlock(&ndev->mmu_lock);
	return ret;
}

int nebulae_mmu_ctx_restore(struct nebulae_device *ndev,
			    struct nebulae_file *nfile)
{
	int ret;

	if (!nfile->asid)
		return -EINVAL;
	mutex_lock(&ndev->mmu_lock);
	ret = nebulae_mmu_ctx_init_locked(ndev, nfile);
	if (!ret)
		neb_mmu_invalidate_tlbs(ndev);
	mutex_unlock(&ndev->mmu_lock);
	return ret;
}

int nebulae_mmu_map(struct nebulae_device *ndev, struct nebulae_file *nfile,
		    u64 va, u64 phys, u64 size, u32 prot)
{
	int ret;

	if (!nfile || !nfile->asid || !nfile->mmu_root)
		return -EINVAL;
	mutex_lock(&ndev->mmu_lock);
	ret = neb_mmu_map_range_locked(ndev, nfile, va, phys, size, prot);
	if (!ret)
		neb_mmu_invalidate_tlbs(ndev);
	mutex_unlock(&ndev->mmu_lock);
	if (ret)
		drm_dbg(&ndev->drm,
			"MMU map failed asid=%u va=0x%llx phys=0x%llx size=0x%llx prot=0x%x: %d\n",
			nfile->asid, va, phys, size, prot, ret);
	return ret;
}

void nebulae_mmu_unmap(struct nebulae_device *ndev,
		       struct nebulae_file *nfile, u64 va, u64 size)
{
	if (!nfile || !nfile->asid || !nfile->mmu_root)
		return;
	mutex_lock(&ndev->mmu_lock);
	neb_mmu_unmap_range_locked(ndev, nfile, va, size);
	wmb();
	neb_mmu_invalidate_tlbs(ndev);
	mutex_unlock(&ndev->mmu_lock);
}

void nebulae_mmu_ctx_free(struct nebulae_device *ndev, u32 asid)
{
	u32 slot;

	if (!asid || asid > NEB_MMU_MAX_CTX)
		return;
	slot = asid - 1;

	mutex_lock(&ndev->mmu_lock);
	if (!READ_ONCE(ndev->unplugged))
		memset_io(ndev->vram + ndev->mmu_pool_base +
			  (u64)slot * NEB_MMU_CTX_PT_BYTES, 0,
			  NEB_MMU_CTX_PT_BYTES);
	neb_mmu_invalidate_tlbs(ndev);
	__clear_bit(slot, ndev->mmu_ctx_bitmap);
	mutex_unlock(&ndev->mmu_lock);
}
