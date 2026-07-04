// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae GPU MMU / page-table management.
 *
 * Implements the Nebula ISA MMU (Nebula_GPU_ABI_Reference_Guide §12D / profile
 * table §12E: "NBL_MMU — 48-bit VA, 16 KiB/64 KiB page, three-level page walk,
 * PTE low-14-bit attributes, ASID, TLB, replayable faults") and §12D.1: "each
 * process / security context owns an independent ASID and page_table_root; the
 * Queue Descriptor binds an ASID and the Command Processor loads the matching
 * address space on packet switch".
 *
 * So the driver manages a *pool of ASIDs*, one per drawing client (per DRM
 * file), each with its own three-level page table.  This mirrors the DRM GPU
 * MMU drivers:
 *   - amdgpu_vm.c   : one VM (page-table tree) per DRM file / context; PTBR is
 *                     the physical base of that VM's root directory.
 *   - panfrost_mmu.c: an address-space (AS) per context, LRU-managed, with
 *                     kref'd page tables; map()/unmap() edit leaf entries.
 *   - pvr_vm.c      : per-context VM contexts, explicit multi-level walk.
 * simplified to the fixed three-level layout the Nebulae hardware / functional
 * simulator (extern/laxpu-nebule-simx cu_tlb.cpp) walks, and to a fixed pool of
 * per-context page-table slots carved from VRAM.
 *
 * Address layout (48-bit VA, 16 KiB pages, 8-byte PTEs):
 *   bits [13:0]   page offset       (16 KiB)
 *   bits [24:14]  L0 (leaf)  index  11 bits, 2048 PTEs -> one 16 KiB table
 *   bits [35:25]  L1 (mid)   index  11 bits, 2048 PTEs -> one 16 KiB table
 *   bits [47:36]  L2 (root)  index  12 bits, 4096 PTEs -> 32 KiB root table
 * PTE: bits [63:14] = 16 KiB-aligned physical (VRAM) base of the next table or
 * (at the leaf) the page frame; low 14 bits = attributes (bit0 = valid).
 */

#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/sizes.h>

#include <drm/drm_print.h>

#include "nebulae_internal.h"

#define NEB_MMU_PAGE_SHIFT	14				/* 16 KiB */
#define NEB_MMU_PAGE_SIZE	(1ULL << NEB_MMU_PAGE_SHIFT)
#define NEB_MMU_PAGE_MASK	(NEB_MMU_PAGE_SIZE - 1)

#define NEB_MMU_L0_SHIFT	NEB_MMU_PAGE_SHIFT		/* 14 */
#define NEB_MMU_L0_BITS		11
#define NEB_MMU_L1_SHIFT	25
#define NEB_MMU_L1_BITS		11
#define NEB_MMU_L2_SHIFT	36
#define NEB_MMU_L2_BITS		12

#define NEB_MMU_L0_ENTRIES	(1U << NEB_MMU_L0_BITS)		/* 2048 */
#define NEB_MMU_L1_ENTRIES	(1U << NEB_MMU_L1_BITS)		/* 2048 */
#define NEB_MMU_L2_ENTRIES	(1U << NEB_MMU_L2_BITS)		/* 4096 */

#define NEB_MMU_L0_SIZE		(NEB_MMU_L0_ENTRIES * sizeof(u64))	/* 16 KiB */
#define NEB_MMU_L1_SIZE		(NEB_MMU_L1_ENTRIES * sizeof(u64))	/* 16 KiB */
#define NEB_MMU_L2_SIZE		(NEB_MMU_L2_ENTRIES * sizeof(u64))	/* 32 KiB */

#define NEB_MMU_VA_BITS		48
#define NEB_MMU_VA_MASK		((1ULL << NEB_MMU_VA_BITS) - 1)

#define NEB_MMU_PTE_ADDR_MASK	(~NEB_MMU_PAGE_MASK)	/* bits [63:14] */
#define NEB_MMU_PTE_VALID	BIT_ULL(0)
#define NEB_MMU_PTE_READ	BIT_ULL(1)
#define NEB_MMU_PTE_WRITE	BIT_ULL(2)
#define NEB_MMU_PTE_EXEC	BIT_ULL(3)
#define NEB_MMU_PTE_USER	BIT_ULL(4)
#define NEB_MMU_PTE_COH		BIT_ULL(5)
#define NEB_MMU_PTE_LEAF_DEFAULT \
	(NEB_MMU_PTE_VALID | NEB_MMU_PTE_READ | NEB_MMU_PTE_WRITE | \
	 NEB_MMU_PTE_EXEC | NEB_MMU_PTE_USER)

/*
 * Per-context page-table slot.  Sized to hold the whole three-level tree for a
 * full identity map of the addressable VRAM: root(32K) + one L1(16K) + a few
 * L0(16K each).  128 KiB is comfortably enough for a 64 MiB window.  ASID 0 is
 * reserved ("no address space / flat"); usable ASIDs are 1..NEB_MMU_MAX_CTX.
 */
#define NEB_MMU_CTX_PT_BYTES	SZ_128K
/* NEB_MMU_MAX_CTX comes from nebulae_internal.h (shared with the ASID bitmap). */
#define NEB_MMU_POOL_SIZE	(NEB_MMU_CTX_PT_BYTES * NEB_MMU_MAX_CTX)

static u64 neb_mmu_l2_index(u64 va) { return (va >> NEB_MMU_L2_SHIFT) & (NEB_MMU_L2_ENTRIES - 1); }
static u64 neb_mmu_l1_index(u64 va) { return (va >> NEB_MMU_L1_SHIFT) & (NEB_MMU_L1_ENTRIES - 1); }
static u64 neb_mmu_l0_index(u64 va) { return (va >> NEB_MMU_L0_SHIFT) & (NEB_MMU_L0_ENTRIES - 1); }

static u64 neb_mmu_read_pte(struct nebulae_device *ndev, u64 table, u64 index)
{
	return readq(ndev->vram + table + index * sizeof(u64));
}

static void neb_mmu_write_pte(struct nebulae_device *ndev, u64 table, u64 index,
			      u64 pte)
{
	writeq(pte, ndev->vram + table + index * sizeof(u64));
}

/* Bump-allocate a zeroed, 16 KiB-aligned table within one context PT slot. */
static int neb_mmu_slot_alloc_table(struct nebulae_device *ndev, u64 slot_base,
				    u64 *bump, u64 size, u64 *out)
{
	u64 off = ALIGN(*bump, NEB_MMU_PAGE_SIZE);

	if (off + size > slot_base + NEB_MMU_CTX_PT_BYTES)
		return -ENOSPC;
	memset_io(ndev->vram + off, 0, size);
	*bump = off + size;
	*out = off;
	return 0;
}

/*
 * Identity-map [va, va+size) into the three-level tree rooted at @root,
 * allocating intermediate L1/L0 tables on demand from the context PT slot
 * (bump within [slot_base, slot_base + NEB_MMU_CTX_PT_BYTES)).  va/size are
 * rounded out to the 16 KiB GPU page.  Leaf PTEs map each GPU VA to the same
 * VRAM offset (identity), so a client sees its own bound VAs and nothing else.
 */
static int neb_mmu_map_range(struct nebulae_device *ndev, u64 root,
			     u64 slot_base, u64 *bump, u64 va, u64 size)
{
	u64 end = ALIGN(va + size, NEB_MMU_PAGE_SIZE);
	int ret;

	va = ALIGN_DOWN(va, NEB_MMU_PAGE_SIZE);
	if (end > NEB_MMU_VA_MASK)
		return -EINVAL;

	for (; va < end; va += NEB_MMU_PAGE_SIZE) {
		u64 pte, l1, l0;

		pte = neb_mmu_read_pte(ndev, root, neb_mmu_l2_index(va));
		if (pte & NEB_MMU_PTE_VALID) {
			l1 = pte & NEB_MMU_PTE_ADDR_MASK;
		} else {
			ret = neb_mmu_slot_alloc_table(ndev, slot_base, bump,
						       NEB_MMU_L1_SIZE, &l1);
			if (ret)
				return ret;
			neb_mmu_write_pte(ndev, root, neb_mmu_l2_index(va),
					  l1 | NEB_MMU_PTE_VALID);
		}

		pte = neb_mmu_read_pte(ndev, l1, neb_mmu_l1_index(va));
		if (pte & NEB_MMU_PTE_VALID) {
			l0 = pte & NEB_MMU_PTE_ADDR_MASK;
		} else {
			ret = neb_mmu_slot_alloc_table(ndev, slot_base, bump,
						       NEB_MMU_L0_SIZE, &l0);
			if (ret)
				return ret;
			neb_mmu_write_pte(ndev, l1, neb_mmu_l1_index(va),
					  l0 | NEB_MMU_PTE_VALID);
		}

		neb_mmu_write_pte(ndev, l0, neb_mmu_l0_index(va),
				  (va & NEB_MMU_PTE_ADDR_MASK) |
					  NEB_MMU_PTE_LEAF_DEFAULT);
	}

	return 0;
}

int nebulae_mmu_init(struct nebulae_device *ndev)
{
	u64 pool_base;
	int ret;

	mutex_init(&ndev->mmu_lock);
	bitmap_zero(ndev->mmu_ctx_bitmap, NEB_MMU_MAX_CTX);

	if (ndev->vram_size <= NEB_MMU_POOL_SIZE + ndev->vm_start)
		return -EINVAL;

	/* Reserve the per-context PT pool at the top of VRAM so the BO VA
	 * allocator never hands it out. */
	pool_base = ALIGN_DOWN(ndev->vram_size - NEB_MMU_POOL_SIZE,
			       NEB_MMU_PAGE_SIZE);
	ndev->mmu_pt_node.start = pool_base;
	ndev->mmu_pt_node.size = ndev->vram_size - pool_base;
	ret = drm_mm_reserve_node(&ndev->va_mm, &ndev->mmu_pt_node);
	if (ret)
		return ret;

	ndev->mmu_pool_base = pool_base;
	ndev->mmu_pool_size = ndev->vram_size - pool_base;

	drm_dbg(&ndev->drm,
		"MMU: 3-level 16KiB pages, %d ASIDs, PT pool [0x%llx,0x%llx), identity [0,0x%llx)\n",
		NEB_MMU_MAX_CTX, ndev->mmu_pool_base,
		ndev->mmu_pool_base + ndev->mmu_pool_size, ndev->mmu_pool_base);
	return 0;
}

void nebulae_mmu_fini(struct nebulae_device *ndev)
{
	if (drm_mm_node_allocated(&ndev->mmu_pt_node))
		drm_mm_remove_node(&ndev->mmu_pt_node);
}

/*
 * Allocate an ASID + its own (initially near-empty) page table for a drawing
 * client (DRM file).  The table maps only the shared low region [0,
 * NEB_SCANOUT_RESERVED) -- display framebuffer, scratch and implicit device
 * regions that every client's waves may touch -- so those never fault under the
 * otherwise sparse address space.  The client's BOs, allocated above that in
 * the VA range, are mapped per-client on bind (nebulae_mmu_map), so one client
 * cannot reach another client's BOs: cross-context access faults.  Populates
 * nfile->{asid, mmu_root (== CSR.PTBR), mmu_slot_base, mmu_bump}.
 */
int nebulae_mmu_ctx_alloc(struct nebulae_device *ndev, struct nebulae_file *nfile)
{
	u64 slot_base, bump, root;
	int slot, ret;

	mutex_lock(&ndev->mmu_lock);
	slot = find_first_zero_bit(ndev->mmu_ctx_bitmap, NEB_MMU_MAX_CTX);
	if (slot >= NEB_MMU_MAX_CTX) {
		mutex_unlock(&ndev->mmu_lock);
		return -ENOSPC;
	}

	slot_base = ndev->mmu_pool_base + (u64)slot * NEB_MMU_CTX_PT_BYTES;
	bump = slot_base;
	ret = neb_mmu_slot_alloc_table(ndev, slot_base, &bump, NEB_MMU_L2_SIZE,
				       &root);
	if (!ret)
		ret = neb_mmu_map_range(ndev, root, slot_base, &bump, 0,
					NEB_SCANOUT_RESERVED);
	if (ret) {
		mutex_unlock(&ndev->mmu_lock);
		return ret;
	}

	__set_bit(slot, ndev->mmu_ctx_bitmap);
	nfile->asid = (u32)slot + 1;	/* ASID 0 reserved for flat/no-VM */
	nfile->mmu_root = root;
	nfile->mmu_slot_base = slot_base;
	nfile->mmu_bump = bump;
	mutex_unlock(&ndev->mmu_lock);
	return 0;
}

/* Map a bound BO's VA range into this client's address space (identity). */
int nebulae_mmu_map(struct nebulae_device *ndev, struct nebulae_file *nfile,
		    u64 va, u64 size)
{
	int ret;

	if (!nfile->asid || !nfile->mmu_root)	/* flat/no-VM client */
		return 0;

	mutex_lock(&ndev->mmu_lock);
	ret = neb_mmu_map_range(ndev, nfile->mmu_root, nfile->mmu_slot_base,
				&nfile->mmu_bump, va, size);
	mutex_unlock(&ndev->mmu_lock);
	if (ret)
		drm_warn(&ndev->drm,
			 "MMU: asid=%u map va=0x%llx size=0x%llx failed: %d (PT slot full?)\n",
			 nfile->asid, va, size, ret);
	return ret;
}

/* Clear leaf PTEs for a BO's VA range in this client's table (intermediate
 * tables are left allocated in the slot). */
void nebulae_mmu_unmap(struct nebulae_device *ndev, struct nebulae_file *nfile,
		       u64 va, u64 size)
{
	u64 end = ALIGN(va + size, NEB_MMU_PAGE_SIZE);

	if (!nfile->asid || !nfile->mmu_root)
		return;
	va = ALIGN_DOWN(va, NEB_MMU_PAGE_SIZE);

	mutex_lock(&ndev->mmu_lock);
	for (; va < end; va += NEB_MMU_PAGE_SIZE) {
		u64 pte = neb_mmu_read_pte(ndev, nfile->mmu_root,
					   neb_mmu_l2_index(va));
		u64 l1, l0;

		if (!(pte & NEB_MMU_PTE_VALID))
			continue;
		l1 = pte & NEB_MMU_PTE_ADDR_MASK;
		pte = neb_mmu_read_pte(ndev, l1, neb_mmu_l1_index(va));
		if (!(pte & NEB_MMU_PTE_VALID))
			continue;
		l0 = pte & NEB_MMU_PTE_ADDR_MASK;
		neb_mmu_write_pte(ndev, l0, neb_mmu_l0_index(va), 0);
	}
	mutex_unlock(&ndev->mmu_lock);
}

void nebulae_mmu_ctx_free(struct nebulae_device *ndev, u32 asid)
{
	u32 slot;

	if (asid == 0 || asid > NEB_MMU_MAX_CTX)
		return;
	slot = asid - 1;

	mutex_lock(&ndev->mmu_lock);
	__clear_bit(slot, ndev->mmu_ctx_bitmap);
	mutex_unlock(&ndev->mmu_lock);
}
