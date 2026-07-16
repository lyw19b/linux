/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEBULAE_INTERNAL_H
#define NEBULAE_INTERNAL_H

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/gpu_scheduler.h>

#include <uapi/drm/nebulae_drm.h>

#include "nebulae_regs.h"

#define DRIVER_NAME	"nebulae"
#define DRIVER_DESC	"Nebulae DRM graphics/display driver"
#define DRIVER_DATE	"20260630"

#define NEB_SCANOUT_RESERVED	SZ_16M
#define NEB_GPU_PAGE_SIZE	SZ_16K
#define NEB_GPU_VA_BITS		48
#define NEB_GPU_VA_START	SZ_16M
#define NEB_GPU_VA_SIZE		((1ULL << NEB_GPU_VA_BITS) - NEB_GPU_VA_START)

#define NEB_VM_PROT_READ	BIT(0)
#define NEB_VM_PROT_WRITE	BIT(1)
#define NEB_VM_PROT_EXEC	BIT(2)
#define NEB_VM_PROT_USER	BIT(3)

struct drm_mode_create_dumb;
struct nebulae_bo;
struct nebulae_vma;
struct nebulae_job;
struct nebulae_submit_shadow;

#define NEB_KMS_PREFERRED_WIDTH		1024
#define NEB_KMS_PREFERRED_HEIGHT	768
#define NEB_KMS_MAX_WIDTH		1920
#define NEB_KMS_MAX_HEIGHT		1080

/* Number of GPU address spaces (ASIDs / per-client page tables) the driver
 * manages; ASID 0 is reserved for flat/no-VM, usable ASIDs are 1..this. */
#define NEB_MMU_MAX_CTX			16
#define NEB_FAULT_QUEUE_DEPTH		16

struct nebulae_file {
	struct kref refcount;
	struct nebulae_device *ndev;
	struct list_head device_link;
	u64 ctx_id;
	u32 asid;		/* GPU address-space id for this client */
	u64 mmu_root;		/* this client's page-table root (CSR.PTBR) */
	u64 mmu_slot_base;	/* base of this client's PT slot in VRAM */
	DECLARE_BITMAP(mmu_pt_bitmap, 32); /* 16 KiB pages in the PT slot */
	atomic64_t submits;
	atomic64_t next_job_seq;
	struct drm_sched_entity sched_entity;
	/* drm_sched_entity is SPSC. Serialize the arm/commit/push transaction and
	 * reject new work once file teardown begins. */
	struct mutex submit_lock;
	bool closing;
	/* Per-file job namespace.  Hardware sequence numbers are device-global and
	 * must never be accepted as user handles without an ownership check. */
	struct xarray jobs;
	/* A GPU VA belongs to a VM/file, not to a BO globally. */
	struct mutex vm_lock;
	struct drm_mm va_mm;
	struct list_head vmas;
	spinlock_t fault_lock;
	struct drm_nebulae_fault faults[NEB_FAULT_QUEUE_DEPTH];
	u32 fault_head;
	u32 fault_count;
	u64 fault_sequence;
};

struct nebulae_vma {
	struct drm_mm_node node;
	struct list_head vm_link;
	struct nebulae_file *nfile;
	struct nebulae_bo *bo; /* NULL for a kernel-private shadow mapping */
	u64 phys;
	u32 prot;
	u32 job_refs;
	bool pending_unmap;
};

struct nebulae_device {
	struct drm_device drm;
	struct platform_device *pdev;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	struct hrtimer vblank_timer;
	ktime_t vblank_period;
	void __iomem *regs;
	void __iomem *vram;
	phys_addr_t vram_phys;	/* physical base of the device VRAM aperture */
	u64 vram_size;
	u64 vm_start;
	u64 vm_size;
	u32 version;
	u32 hw_caps;
	struct drm_nebulae_device_info device_info;
	u32 last_error;
	int irq;
	struct mutex bo_lock;
	struct mutex submit_lock;
	struct mutex files_lock;
	struct mutex reset_lock;
	wait_queue_head_t submit_wait;
	wait_queue_head_t active_op_wait;
	spinlock_t fence_lock;
	spinlock_t hw_lock;
	/* Protects the KMS-to-submit ownership snapshot below. */
	spinlock_t scanout_lock;
	u64 scanout_base;
	u64 scanout_size;
	u64 scanout_generation;
	bool scanout_direct;
	struct drm_gpu_scheduler scheduler;
	struct nebulae_job *active_job;
	struct work_struct irq_work;
	struct work_struct reset_work;
	atomic_t pending_irq;
	atomic_t reset_pending;
	atomic_t active_ops;
	struct list_head bo_list;
	struct list_head files;
	/* Global VRAM storage allocator.  GPU VAs live in each file's va_mm. */
	struct drm_mm vram_mm;
	/* GPU MMU: a pool of per-ASID three-level page tables in VRAM, one page
	 * table per drawing client / DRM file (see nebulae_mmu.c). */
	struct mutex mmu_lock;
	struct drm_mm_node mmu_pt_node;	/* PT pool reserved in vram_mm */
	u64 mmu_pool_base;
	u64 mmu_pool_size;
	DECLARE_BITMAP(mmu_ctx_bitmap, NEB_MMU_MAX_CTX);	/* allocated ASIDs */
	u64 fence_context;
	bool sysfs_registered;
	atomic64_t submitted_jobs;
	atomic64_t completed_jobs;
	atomic64_t fence_seqno;
	atomic64_t signaled_fences;
	atomic64_t next_ctx_id;
	atomic64_t open_contexts;
	atomic64_t scheduled_jobs;
	atomic64_t running_jobs;
	atomic64_t finished_jobs;
	atomic64_t failed_jobs;
	atomic64_t irq_count;
	atomic64_t complete_irq_count;
	atomic64_t fault_irq_count;
	atomic64_t display_irq_count;
	atomic64_t display_flips;
	u64 last_submit_cookie;
	u64 last_submit_pt_base;
	u32 last_submit_asid;
	u32 last_irq_status;
	u32 last_display_irq_status;
	spinlock_t fault_lock;
	struct drm_nebulae_fault last_fault;
	atomic64_t reset_count;
	u32 last_reset_reason;
	u64 last_reset_timestamp_ns;
	bool resetting;
	bool suspended;
	bool system_suspended;
	bool wedged;
	bool display_pm_ref;
	bool unplugged;
};

struct nebulae_bo {
	struct drm_gem_shmem_object base;
	struct list_head link;
	struct drm_mm_node vram_node;
	u64 vram_offset;
	u32 flags;
	u32 domain;
	bool listed;
};

struct nebulae_job {
	struct drm_sched_job base;
	struct kref refcount;
	struct nebulae_device *ndev;
	struct nebulae_file *nfile;
	struct drm_gem_object *cmd_obj;
	struct nebulae_bo *cmd_bo;
	struct drm_gem_object **objs;
	u32 obj_count;
	struct nebulae_vma **vmas;
	u32 vma_count;
	u32 offset;
	u32 size;
	u32 cmd_count;
	u32 asid;
	u64 pt_base;
	u64 cookie;
	u64 hw_seq;
	u64 user_seq;
	u64 cq_phys;
	u32 hw_status;
	int result;
	bool job_registered;
	struct dma_fence *hw_fence;
	struct nebulae_submit_shadow *shadow;
	bool pm_ref;
	bool nfile_ref;
};

static inline struct nebulae_device *to_nebulae(struct drm_device *drm)
{
	return container_of(drm, struct nebulae_device, drm);
}

static inline struct nebulae_bo *to_nebulae_bo(struct drm_gem_object *obj)
{
	return container_of(obj, struct nebulae_bo, base.base);
}

static inline u64 neb_readq(struct nebulae_device *ndev, u32 lo_reg)
{
	u32 lo = readl(ndev->regs + lo_reg);
	u32 hi = readl(ndev->regs + lo_reg + 4);

	return ((u64)hi << 32) | lo;
}

static inline void neb_writeq(struct nebulae_device *ndev, u32 lo_reg,
			      u64 value)
{
	writel((u32)value, ndev->regs + lo_reg);
	writel((u32)(value >> 32), ndev->regs + lo_reg + 4);
}

extern const struct drm_driver nebulae_gpu_drm_driver;

int nebulae_device_probe(struct platform_device *pdev);
void nebulae_device_remove(struct platform_device *pdev);
void nebulae_device_shutdown(struct platform_device *pdev);
int nebulae_device_enter(struct nebulae_device *ndev, int *idx);
void nebulae_device_exit(struct nebulae_device *ndev, int idx);

struct drm_gem_object *nebulae_gpu_gem_create_object(struct drm_device *drm,
						     size_t size);
int nebulae_ioctl_bo_create(struct drm_device *drm, void *data,
			    struct drm_file *file);
int nebulae_dumb_create(struct drm_file *file, struct drm_device *drm,
			struct drm_mode_create_dumb *args);
int nebulae_ioctl_bo_mmap(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_bo_wait(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_madvise(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_bo_info(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_bo_set_domain(struct drm_device *drm, void *data,
				struct drm_file *file);
int nebulae_ioctl_vm_bind(struct drm_device *drm, void *data,
			  struct drm_file *file);
struct drm_gem_object *nebulae_gem_prime_import(struct drm_device *drm,
						struct dma_buf *dma_buf);
int nebulae_bo_sync_to_vram(struct nebulae_device *ndev,
			    struct nebulae_bo *bo);
int nebulae_bo_sync_to_vram_nowait(struct nebulae_device *ndev,
				   struct nebulae_bo *bo);
int nebulae_bo_sync_from_vram(struct nebulae_device *ndev,
			      struct nebulae_bo *bo);
int nebulae_bo_sync_from_vram_nowait(struct nebulae_device *ndev,
				     struct nebulae_bo *bo);

int nebulae_vm_init(struct nebulae_device *ndev);
void nebulae_vm_fini(struct nebulae_device *ndev);
int nebulae_vm_file_init(struct nebulae_file *nfile);
void nebulae_vm_file_fini(struct nebulae_device *ndev,
			  struct nebulae_file *nfile);
int nebulae_vm_map_bo(struct nebulae_device *ndev,
		      struct nebulae_file *nfile, struct nebulae_bo *bo,
		      u64 *va);
int nebulae_vm_unmap_bo(struct nebulae_device *ndev,
			struct nebulae_file *nfile, struct nebulae_bo *bo,
			bool defer_busy);
int nebulae_vm_bo_va(struct nebulae_file *nfile, struct nebulae_bo *bo,
		     u64 *va);
int nebulae_vm_job_pin_bos(struct nebulae_file *nfile,
			   struct drm_gem_object **objs, u32 obj_count,
			   struct nebulae_vma ***vmas, u32 *vma_count);
void nebulae_vm_job_unpin_bos(struct nebulae_device *ndev,
			      struct nebulae_file *nfile,
			      struct nebulae_vma ***vmas, u32 *vma_count);
int nebulae_vm_map_kernel(struct nebulae_device *ndev,
			  struct nebulae_file *nfile, u64 phys, u64 size,
			  u32 prot, struct nebulae_vma **out_vma);
void nebulae_vm_unmap_kernel(struct nebulae_device *ndev,
			     struct nebulae_file *nfile,
			     struct nebulae_vma *vma);
bool nebulae_vm_range_valid(struct nebulae_file *nfile, u64 va, u64 size,
			    u32 prot, struct nebulae_bo **bo_out);
int nebulae_vm_restore(struct nebulae_device *ndev,
		       struct nebulae_file *nfile);

int nebulae_mmu_init(struct nebulae_device *ndev);
void nebulae_mmu_fini(struct nebulae_device *ndev);
int nebulae_mmu_ctx_alloc(struct nebulae_device *ndev,
			  struct nebulae_file *nfile);
void nebulae_mmu_ctx_free(struct nebulae_device *ndev, u32 asid);
int nebulae_mmu_map(struct nebulae_device *ndev, struct nebulae_file *nfile,
		    u64 va, u64 phys, u64 size, u32 prot);
void nebulae_mmu_unmap(struct nebulae_device *ndev, struct nebulae_file *nfile,
		       u64 va, u64 size);
int nebulae_mmu_ctx_restore(struct nebulae_device *ndev,
			    struct nebulae_file *nfile);
int nebulae_alloc_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo,
			u64 size);
void nebulae_free_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo);

int nebulae_ctx_init(struct nebulae_device *ndev);
void nebulae_ctx_fini(struct nebulae_device *ndev);
int nebulae_file_open(struct drm_device *drm, struct drm_file *file);
void nebulae_file_postclose(struct drm_device *drm, struct drm_file *file);
bool nebulae_file_get(struct nebulae_file *nfile);
void nebulae_file_put(struct nebulae_file *nfile);

int nebulae_sched_init(struct nebulae_device *ndev);
void nebulae_gpu_sched_fini(struct nebulae_device *ndev);
void nebulae_sched_record_submit(struct nebulae_device *ndev);
void nebulae_sched_record_complete(struct nebulae_device *ndev, int ret);
extern const struct drm_sched_backend_ops nebulae_gpu_sched_ops;

int nebulae_ioctl_submit(struct drm_device *drm, void *data,
			 struct drm_file *file);
int nebulae_ioctl_submit_cmd_bo(struct drm_device *drm, void *data,
				struct drm_file *file);
int nebulae_ioctl_job_control(struct drm_device *drm, void *data,
			      struct drm_file *file);

irqreturn_t nebulae_gpu_irq(int irq, void *data);
void nebulae_irq_init(struct nebulae_device *ndev);
void nebulae_irq_fini(struct nebulae_device *ndev);
void nebulae_submit_irq_process(struct nebulae_device *ndev, u32 status);
void nebulae_submit_abort_active(struct nebulae_device *ndev, int error,
				 u32 reason);
void nebulae_submit_file_kill_active(struct nebulae_device *ndev,
				     struct nebulae_file *nfile);

void nebulae_recovery_init(struct nebulae_device *ndev);
void nebulae_recovery_fini(struct nebulae_device *ndev);
void nebulae_schedule_reset(struct nebulae_device *ndev, u32 reason);
int nebulae_device_reset(struct nebulae_device *ndev,
			 struct drm_sched_job *bad, u32 reason);
int nebulae_runtime_suspend(struct device *dev);
int nebulae_runtime_resume(struct device *dev);
int nebulae_system_suspend(struct device *dev);
int nebulae_system_resume(struct device *dev);
void nebulae_fault_record(struct nebulae_device *ndev,
			  struct nebulae_job *job, u32 reason, u32 access,
			  u32 flags, u64 va, u32 hw_status, int error);
int nebulae_ioctl_get_fault(struct drm_device *drm, void *data,
			    struct drm_file *file);

void nebulae_fill_device_info(struct nebulae_device *ndev,
			      struct drm_nebulae_device_info *info);
int nebulae_get_param_value(struct nebulae_device *ndev, u32 param,
			    u64 *value);
int nebulae_ioctl_get_param(struct drm_device *drm, void *data,
			    struct drm_file *file);
int nebulae_ioctl_get_info(struct drm_device *drm, void *data,
			   struct drm_file *file);

int nebulae_kms_init(struct nebulae_device *ndev);
u64 nebulae_scanout_size(unsigned int width, unsigned int height,
			 unsigned int pitch);
int nebulae_crtc_init(struct nebulae_device *ndev);
int nebulae_output_init(struct nebulae_device *ndev);
bool nebulae_fb_supported(const struct drm_framebuffer *fb);
struct drm_framebuffer *nebulae_fb_create(struct drm_device *drm,
					  struct drm_file *file,
					  const struct drm_mode_fb_cmd2 *mode_cmd);
int nebulae_plane_check(struct drm_simple_display_pipe *pipe,
			struct drm_plane_state *plane_state,
			struct drm_crtc_state *crtc_state);
void nebulae_plane_enable(struct drm_simple_display_pipe *pipe,
			  struct drm_crtc_state *crtc_state,
			  struct drm_plane_state *plane_state);
void nebulae_plane_disable(struct drm_simple_display_pipe *pipe);
void nebulae_plane_update(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *old_plane_state);
int nebulae_vblank_init(struct nebulae_device *ndev);
void nebulae_vblank_fini(struct nebulae_device *ndev);
int nebulae_vblank_enable(struct drm_simple_display_pipe *pipe);
void nebulae_vblank_disable(struct drm_simple_display_pipe *pipe);
void nebulae_vblank_record_flip(struct nebulae_device *ndev);

void nebulae_debugfs_init(struct drm_minor *minor);
int nebulae_gpu_sysfs_init(struct nebulae_device *ndev);
void nebulae_gpu_sysfs_fini(struct nebulae_device *ndev);

#endif /* NEBULAE_INTERNAL_H */
