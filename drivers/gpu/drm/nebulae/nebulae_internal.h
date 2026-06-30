/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef NEBULAE_INTERNAL_H
#define NEBULAE_INTERNAL_H

#include <linux/atomic.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_mm.h>
#include <drm/drm_simple_kms_helper.h>

#include <uapi/drm/nebulae_drm.h>

#include "nebulae_regs.h"

#define DRIVER_NAME	"nebulae"
#define DRIVER_DESC	"Nebulae DRM graphics/display driver"
#define DRIVER_DATE	"20260630"

#define NEB_SCANOUT_RESERVED	SZ_16M

#define NEB_KMS_PREFERRED_WIDTH		1024
#define NEB_KMS_PREFERRED_HEIGHT	768
#define NEB_KMS_MAX_WIDTH		1920
#define NEB_KMS_MAX_HEIGHT		1080

struct nebulae_file {
	u64 ctx_id;
	atomic64_t submits;
};

struct nebulae_device {
	struct drm_device drm;
	struct platform_device *pdev;
	struct drm_simple_display_pipe pipe;
	struct drm_connector connector;
	void __iomem *regs;
	void __iomem *vram;
	u64 vram_size;
	u64 vm_start;
	u64 vm_size;
	u32 version;
	u32 hw_caps;
	u32 last_error;
	int irq;
	struct mutex bo_lock;
	struct mutex submit_lock;
	struct list_head bo_list;
	struct drm_mm va_mm;
	u64 next_va;
	bool sysfs_registered;
	atomic64_t submitted_jobs;
	atomic64_t completed_jobs;
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
	u32 last_irq_status;
	u32 last_display_irq_status;
};

struct nebulae_bo {
	struct drm_gem_shmem_object base;
	struct list_head link;
	struct drm_mm_node va_node;
	u64 va;
	u32 flags;
	bool listed;
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

extern const struct drm_driver nebulae_drm_driver;

int nebulae_device_probe(struct platform_device *pdev);
void nebulae_device_remove(struct platform_device *pdev);
void nebulae_device_shutdown(struct platform_device *pdev);

struct drm_gem_object *nebulae_gem_create_object(struct drm_device *drm,
						 size_t size);
int nebulae_ioctl_bo_create(struct drm_device *drm, void *data,
			    struct drm_file *file);
int nebulae_ioctl_bo_mmap(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_bo_wait(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_ioctl_madvise(struct drm_device *drm, void *data,
			  struct drm_file *file);
int nebulae_sync_all_bos_to_vram(struct nebulae_device *ndev);
int nebulae_sync_all_bos_from_vram(struct nebulae_device *ndev);

int nebulae_vm_init(struct nebulae_device *ndev);
void nebulae_vm_fini(struct nebulae_device *ndev);
int nebulae_alloc_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo,
			u64 size);
void nebulae_free_bo_va(struct nebulae_device *ndev, struct nebulae_bo *bo);

int nebulae_ctx_init(struct nebulae_device *ndev);
void nebulae_ctx_fini(struct nebulae_device *ndev);
int nebulae_file_open(struct drm_device *drm, struct drm_file *file);
void nebulae_file_postclose(struct drm_device *drm, struct drm_file *file);

int nebulae_sched_init(struct nebulae_device *ndev);
void nebulae_sched_fini(struct nebulae_device *ndev);
void nebulae_sched_record_submit(struct nebulae_device *ndev);
void nebulae_sched_record_complete(struct nebulae_device *ndev, int ret);

int nebulae_ioctl_submit(struct drm_device *drm, void *data,
			 struct drm_file *file);
int nebulae_ioctl_submit_cmd_bo(struct drm_device *drm, void *data,
				struct drm_file *file);

irqreturn_t nebulae_irq(int irq, void *data);

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
void nebulae_vblank_record_flip(struct nebulae_device *ndev);

void nebulae_debugfs_init(struct drm_minor *minor);
int nebulae_sysfs_init(struct nebulae_device *ndev);
void nebulae_sysfs_fini(struct nebulae_device *ndev);

#endif /* NEBULAE_INTERNAL_H */
