// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae DRM graphics/display driver.
 *
 * This driver is independent from drivers/accel/nebulae.  It exposes the
 * Mesa-facing render/KMS device nodes while the accel driver remains dedicated
 * to accelerator compute workloads.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/dma-direction.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#include <asm/unaligned.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_file.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_managed.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>
#include <drm/drm_simple_kms_helper.h>

#include <uapi/drm/nebulae_drm.h>

#include "nebulae_regs.h"

#define DRIVER_NAME	"nebulae"
#define DRIVER_DESC	"Nebulae DRM graphics/display driver"
#define DRIVER_DATE	"20260630"

#define NEB_SCANOUT_RESERVED	SZ_16M
#define NEB_INTERNAL_CMD_OFFSET	(NEB_SCANOUT_RESERVED - PAGE_SIZE)
#define NEB_USER_CMD_SIZE	128
#define NEB_USER_CMD_CP_EXEC	3
#define NEB_USER_CMD_CP_RING_BASE	16
#define NEB_USER_CMD_CP_RING_SIZE	24
#define NEB_USER_CMD_CP_PACKET_OFFSET	32
#define NEB_USER_CMD_CP_PACKET_SIZE	40
#define NEB_USER_CMD_CP_PACKET_COUNT	44
#define NEB_USER_CMD_CP_PT_BASE		64
#define NEB_USER_CMD_CP_ASID		72

#define NEB_KMS_PREFERRED_WIDTH	1024
#define NEB_KMS_PREFERRED_HEIGHT	768
#define NEB_KMS_MAX_WIDTH	1920
#define NEB_KMS_MAX_HEIGHT	1080

#define NEB_FEATURE_CORE	BIT_ULL(0)
#define NEB_FEATURE_GRAPHICS	BIT_ULL(1)
#define NEB_FEATURE_COMPUTE	BIT_ULL(2)
#define NEB_FEATURE_MEMORY	BIT_ULL(3)
#define NEB_FEATURE_TEXTURE	BIT_ULL(4)
#define NEB_FEATURE_MMU	BIT_ULL(15)
#define NEB_SUPPORTED_FEATURES	(NEB_FEATURE_CORE | NEB_FEATURE_GRAPHICS | \
				 NEB_FEATURE_COMPUTE | NEB_FEATURE_MEMORY | \
				 NEB_FEATURE_TEXTURE | NEB_FEATURE_MMU)

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
	u64 next_va;
	atomic64_t submitted_jobs;
	atomic64_t completed_jobs;
};

struct nebulae_bo {
	struct drm_gem_shmem_object base;
	struct list_head link;
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

static const struct drm_gem_object_funcs nebulae_gem_object_funcs;

static u64 neb_readq(struct nebulae_device *ndev, u32 lo_reg)
{
	u32 lo = readl(ndev->regs + lo_reg);
	u32 hi = readl(ndev->regs + lo_reg + 4);

	return ((u64)hi << 32) | lo;
}

static void neb_writeq(struct nebulae_device *ndev, u32 lo_reg, u64 value)
{
	writel((u32)value, ndev->regs + lo_reg);
	writel((u32)(value >> 32), ndev->regs + lo_reg + 4);
}

static struct drm_gem_object *nebulae_gem_create_object(struct drm_device *drm,
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

static void nebulae_fill_device_info(struct nebulae_device *ndev,
				     struct drm_nebulae_device_info *info)
{
	memset(info, 0, sizeof(*info));
	info->isa_major = 4;
	info->isa_minor = 4;
	info->wave_size = 32;
	info->num_compute_units = 1;
	info->vram_size = ndev->vram_size;
	info->max_sr_count = 100;
	info->max_user_sr_count = 96;
	info->max_vr_count = 128;
	info->max_scratch_bytes_per_wave = 512;
	info->max_wgm_bytes_per_workgroup = 32 * 1024;
	info->max_waves_per_cu = 16;
	info->max_workgroup_invocations = 1024;
	info->max_textures = 16;
	info->max_samplers = 16;
	info->max_images = 8;
	info->max_ubos = 16;
	info->max_ssbos = 16;
	info->supported_features = NEB_SUPPORTED_FEATURES;
}

static int nebulae_get_param_value(struct nebulae_device *ndev, u32 param,
				   u64 *value)
{
	struct drm_nebulae_device_info info;

	nebulae_fill_device_info(ndev, &info);

	switch (param) {
	case DRM_NEBULAE_PARAM_ISA_MAJOR:
		*value = info.isa_major;
		break;
	case DRM_NEBULAE_PARAM_ISA_MINOR:
		*value = info.isa_minor;
		break;
	case DRM_NEBULAE_PARAM_WAVE_SIZE:
		*value = info.wave_size;
		break;
	case DRM_NEBULAE_PARAM_NUM_COMPUTE_UNITS:
		*value = info.num_compute_units;
		break;
	case DRM_NEBULAE_PARAM_VRAM_SIZE:
		*value = info.vram_size;
		break;
	case DRM_NEBULAE_PARAM_MAX_SR_COUNT:
		*value = info.max_sr_count;
		break;
	case DRM_NEBULAE_PARAM_MAX_USER_SR_COUNT:
		*value = info.max_user_sr_count;
		break;
	case DRM_NEBULAE_PARAM_MAX_VR_COUNT:
		*value = info.max_vr_count;
		break;
	case DRM_NEBULAE_PARAM_MAX_SCRATCH_BYTES_PER_WAVE:
		*value = info.max_scratch_bytes_per_wave;
		break;
	case DRM_NEBULAE_PARAM_MAX_WGM_BYTES_PER_WORKGROUP:
		*value = info.max_wgm_bytes_per_workgroup;
		break;
	case DRM_NEBULAE_PARAM_MAX_WAVES_PER_CU:
		*value = info.max_waves_per_cu;
		break;
	case DRM_NEBULAE_PARAM_MAX_WORKGROUP_INVOCATIONS:
		*value = info.max_workgroup_invocations;
		break;
	case DRM_NEBULAE_PARAM_MAX_TEXTURES:
		*value = info.max_textures;
		break;
	case DRM_NEBULAE_PARAM_MAX_SAMPLERS:
		*value = info.max_samplers;
		break;
	case DRM_NEBULAE_PARAM_MAX_IMAGES:
		*value = info.max_images;
		break;
	case DRM_NEBULAE_PARAM_MAX_UBOS:
		*value = info.max_ubos;
		break;
	case DRM_NEBULAE_PARAM_MAX_SSBOS:
		*value = info.max_ssbos;
		break;
	case DRM_NEBULAE_PARAM_SUPPORTED_FEATURES:
		*value = info.supported_features;
		break;
	case DRM_NEBULAE_PARAM_UAPI_VERSION:
		*value = DRM_NEBULAE_UAPI_VERSION;
		break;
	case DRM_NEBULAE_PARAM_SUBMIT_CAPS:
		*value = DRM_NEBULAE_SUBMIT_CAP_CMD_BO;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int nebulae_alloc_bo_va(struct nebulae_device *ndev,
			       struct nebulae_bo *bo, u64 size)
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

static int nebulae_sync_all_bos_to_vram(struct nebulae_device *ndev)
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

static int nebulae_sync_all_bos_from_vram(struct nebulae_device *ndev)
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

static void neb_cmd_put_u32(u8 *cmd, size_t off, u32 value)
{
	put_unaligned_le32(value, cmd + off);
}

static void neb_cmd_put_u64(u8 *cmd, size_t off, u64 value)
{
	put_unaligned_le64(value, cmd + off);
}

static int nebulae_hw_submit_cmd_bo(struct nebulae_device *ndev,
				    struct nebulae_bo *cmd_bo,
				    u32 offset, u32 size, u32 cmd_count,
				    u64 *seq, u32 *hw_status)
{
	struct drm_gem_object *obj = &cmd_bo->base.base;
	u8 cmd[NEB_USER_CMD_SIZE];
	u64 ring_size;
	u64 completed;

	if (!size || !cmd_count)
		return -EINVAL;
	if ((u64)offset + size > obj->size)
		return -EINVAL;
	if (!cmd_bo->va)
		return -EINVAL;

	ring_size = obj->size;

	memset(cmd, 0, sizeof(cmd));
	neb_cmd_put_u32(cmd, 0, NEB_USER_CMD_CP_EXEC);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_BASE, cmd_bo->va);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_SIZE, ring_size);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_OFFSET, offset);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_SIZE, size);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_COUNT, cmd_count);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_PT_BASE, 0);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_ASID, 0);

	mutex_lock(&ndev->submit_lock);
	memcpy_toio(ndev->vram + NEB_INTERNAL_CMD_OFFSET, cmd, sizeof(cmd));
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	neb_writeq(ndev, NEB_REG_SUBMIT_OFFSET_LO, NEB_INTERNAL_CMD_OFFSET);
	writel(sizeof(cmd), ndev->regs + NEB_REG_SUBMIT_SIZE);
	writel(1, ndev->regs + NEB_REG_DOORBELL);

	*seq = neb_readq(ndev, NEB_REG_JOB_SEQ_LO);
	completed = neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO);
	*hw_status = readl(ndev->regs + NEB_REG_LAST_ERROR);
	WRITE_ONCE(ndev->last_error, *hw_status);
	atomic64_set(&ndev->submitted_jobs, *seq);
	atomic64_set(&ndev->completed_jobs, completed);
	mutex_unlock(&ndev->submit_lock);

	if (completed != *seq)
		return -ETIMEDOUT;

	return 0;
}

static irqreturn_t nebulae_irq(int irq, void *data)
{
	struct nebulae_device *ndev = data;
	u32 status;

	status = readl(ndev->regs + NEB_REG_IRQ_STATUS);
	if (!status)
		return IRQ_NONE;

	writel(status, ndev->regs + NEB_REG_IRQ_STATUS);
	WRITE_ONCE(ndev->last_error, readl(ndev->regs + NEB_REG_LAST_ERROR));
	atomic64_set(&ndev->completed_jobs,
		     neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO));

	return IRQ_HANDLED;
}

static u64 nebulae_scanout_size(unsigned int width, unsigned int height,
				unsigned int pitch)
{
	if (!width || !height)
		return 0;

	return (u64)(height - 1) * pitch + (u64)width * 4;
}

static bool nebulae_fb_supported(const struct drm_framebuffer *fb)
{
	if (!fb || fb->format->num_planes != 1)
		return false;

	switch (fb->format->format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		return true;
	default:
		return false;
	}
}

static struct drm_framebuffer *
nebulae_fb_create(struct drm_device *drm, struct drm_file *file,
		  const struct drm_mode_fb_cmd2 *mode_cmd)
{
	unsigned int i;

	switch (mode_cmd->pixel_format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		break;
	default:
		return ERR_PTR(-EINVAL);
	}

	if (mode_cmd->flags & DRM_MODE_FB_MODIFIERS) {
		for (i = 0; i < ARRAY_SIZE(mode_cmd->modifier); i++) {
			if (mode_cmd->modifier[i] != DRM_FORMAT_MOD_INVALID &&
			    mode_cmd->modifier[i] != DRM_FORMAT_MOD_LINEAR)
				return ERR_PTR(-EINVAL);
		}
	}

	return drm_gem_fb_create(drm, file, mode_cmd);
}

static int nebulae_connector_get_modes(struct drm_connector *connector)
{
	int count;

	count = drm_add_modes_noedid(connector, NEB_KMS_MAX_WIDTH,
				     NEB_KMS_MAX_HEIGHT);
	drm_set_preferred_mode(connector, NEB_KMS_PREFERRED_WIDTH,
			       NEB_KMS_PREFERRED_HEIGHT);

	return count;
}

static const struct drm_connector_helper_funcs nebulae_connector_helper_funcs = {
	.get_modes = nebulae_connector_get_modes,
};

static const struct drm_connector_funcs nebulae_connector_funcs = {
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static enum drm_mode_status
nebulae_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
			const struct drm_display_mode *mode)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	u64 scanout_size;

	if (!mode->hdisplay || !mode->vdisplay)
		return MODE_BAD;
	if (mode->hdisplay > NEB_KMS_MAX_WIDTH)
		return MODE_VIRTUAL_X;
	if (mode->vdisplay > NEB_KMS_MAX_HEIGHT)
		return MODE_VIRTUAL_Y;

	scanout_size = nebulae_scanout_size(mode->hdisplay, mode->vdisplay,
					    mode->hdisplay * 4);
	if (scanout_size > ndev->vram_size)
		return MODE_MEM;

	return MODE_OK;
}

static int nebulae_pipe_check(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *plane_state,
			      struct drm_crtc_state *crtc_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *obj;
	unsigned int width;
	unsigned int height;
	u64 scanout_size;

	crtc_state->no_vblank = true;

	if (!fb || !plane_state->visible)
		return 0;
	if (!nebulae_fb_supported(fb))
		return -EINVAL;

	width = plane_state->crtc_w ?: fb->width;
	height = plane_state->crtc_h ?: fb->height;
	if (!width || !height)
		return -EINVAL;
	if (width > NEB_KMS_MAX_WIDTH || height > NEB_KMS_MAX_HEIGHT)
		return -EINVAL;
	if (fb->pitches[0] < width * 4)
		return -EINVAL;

	scanout_size = nebulae_scanout_size(width, height, fb->pitches[0]);
	if (!scanout_size || scanout_size > ndev->vram_size)
		return -EINVAL;

	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj || (u64)fb->offsets[0] + scanout_size > obj->size)
		return -EINVAL;

	return 0;
}

static void nebulae_program_display(struct nebulae_device *ndev,
				    unsigned int width, unsigned int height,
				    unsigned int pitch, u64 scanout_size)
{
	writel(0, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	writel(width, ndev->regs + NEB_REG_DISPLAY_WIDTH);
	writel(height, ndev->regs + NEB_REG_DISPLAY_HEIGHT);
	writel(pitch, ndev->regs + NEB_REG_DISPLAY_STRIDE);
	writel(NEB_DISPLAY_FORMAT_XRGB8888,
	       ndev->regs + NEB_REG_DISPLAY_FORMAT);
	neb_writeq(ndev, NEB_REG_DISPLAY_FB_BASE_LO, 0);
	writel((u32)scanout_size, ndev->regs + NEB_REG_DISPLAY_FB_SIZE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_FLIP);
}

static void nebulae_kms_update_scanout(struct nebulae_device *ndev,
				       struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *fb;
	struct iosys_map map[DRM_FORMAT_MAX_PLANES];
	struct iosys_map data[DRM_FORMAT_MAX_PLANES];
	struct iosys_map dst = IOSYS_MAP_INIT_VADDR_IOMEM(ndev->vram);
	unsigned int dst_pitch[DRM_FORMAT_MAX_PLANES] = { 0 };
	unsigned int width;
	unsigned int height;
	u64 scanout_size;
	struct drm_rect clip;
	int idx;
	int ret;

	if (!plane_state || !plane_state->fb)
		return;

	fb = plane_state->fb;
	width = plane_state->crtc_w ?: fb->width;
	height = plane_state->crtc_h ?: fb->height;
	scanout_size = nebulae_scanout_size(width, height, fb->pitches[0]);
	if (!scanout_size || scanout_size > ndev->vram_size)
		return;

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		return;

	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret)
		goto out_cpu_access;
	if (data[0].is_iomem)
		goto out_vunmap;

	if (!drm_dev_enter(&ndev->drm, &idx))
		goto out_vunmap;

	dst_pitch[0] = fb->pitches[0];
	clip = DRM_RECT_INIT(0, 0, width, height);
	drm_fb_memcpy(&dst, dst_pitch, data, fb, &clip);
	nebulae_program_display(ndev, width, height, fb->pitches[0],
				 scanout_size);

	drm_dev_exit(idx);

out_vunmap:
	drm_gem_fb_vunmap(fb, map);
out_cpu_access:
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);
}

static void nebulae_pipe_enable(struct drm_simple_display_pipe *pipe,
				struct drm_crtc_state *crtc_state,
				struct drm_plane_state *plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	nebulae_kms_update_scanout(ndev, plane_state);
}

static void nebulae_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	int idx;

	if (!drm_dev_enter(&ndev->drm, &idx))
		return;

	writel(0, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	drm_dev_exit(idx);
}

static void nebulae_pipe_update(struct drm_simple_display_pipe *pipe,
				struct drm_plane_state *old_plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	nebulae_kms_update_scanout(ndev, pipe->plane.state);
}

static const u32 nebulae_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u64 nebulae_pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

static const struct drm_simple_display_pipe_funcs nebulae_pipe_funcs = {
	.mode_valid = nebulae_pipe_mode_valid,
	.enable = nebulae_pipe_enable,
	.disable = nebulae_pipe_disable,
	.check = nebulae_pipe_check,
	.update = nebulae_pipe_update,
};

static const struct drm_mode_config_funcs nebulae_mode_config_funcs = {
	.fb_create = nebulae_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

static int nebulae_kms_init(struct nebulae_device *ndev)
{
	struct drm_device *drm = &ndev->drm;
	int ret;

	ret = drmm_mode_config_init(drm);
	if (ret)
		return ret;

	drm->mode_config.min_width = 1;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_width = NEB_KMS_MAX_WIDTH;
	drm->mode_config.max_height = NEB_KMS_MAX_HEIGHT;
	drm->mode_config.preferred_depth = 24;
	drm->mode_config.funcs = &nebulae_mode_config_funcs;

	ret = drmm_connector_init(drm, &ndev->connector,
				  &nebulae_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret)
		return ret;

	ndev->connector.polled = DRM_CONNECTOR_POLL_CONNECT;
	drm_connector_helper_add(&ndev->connector,
				 &nebulae_connector_helper_funcs);

	ret = drm_simple_display_pipe_init(drm, &ndev->pipe,
					   &nebulae_pipe_funcs,
					   nebulae_pipe_formats,
					   ARRAY_SIZE(nebulae_pipe_formats),
					   nebulae_pipe_modifiers,
					   &ndev->connector);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}

static int nebulae_ioctl_get_param(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_get_param *args = data;

	if (args->pad)
		return -EINVAL;

	return nebulae_get_param_value(ndev, args->param, &args->value);
}

static int nebulae_ioctl_get_info(struct drm_device *drm, void *data,
				  struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_device_info *args = data;

	nebulae_fill_device_info(ndev, args);

	return 0;
}

static int nebulae_ioctl_bo_create(struct drm_device *drm, void *data,
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

static int nebulae_ioctl_bo_mmap(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct drm_nebulae_bo_mmap_offset *args = data;

	if (args->pad)
		return -EINVAL;

	return drm_gem_dumb_map_offset(file, drm, args->handle, &args->offset);
}

static int nebulae_ioctl_bo_wait(struct drm_device *drm, void *data,
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

static int nebulae_ioctl_submit(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct drm_nebulae_submit *args = data;

	if (!args->cmds || !args->size)
		return -EINVAL;
	if (args->flags)
		return -EOPNOTSUPP;

	return -EOPNOTSUPP;
}

static int nebulae_ioctl_madvise(struct drm_device *drm, void *data,
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

static int nebulae_ioctl_submit_cmd_bo(struct drm_device *drm, void *data,
				       struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_submit_cmd_bo *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	u32 unsupported_flags = DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ |
				DRM_NEBULAE_SUBMIT_IN_FENCE_FD |
				DRM_NEBULAE_SUBMIT_OUT_FENCE_FD;
	u32 hw_status = 0;
	u64 seq;
	int ret;

	if (args->flags & unsupported_flags)
		return -EOPNOTSUPP;
	if (!args->handle || !args->size || !args->cmd_count)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if ((u64)args->offset + args->size > obj->size) {
		ret = -EINVAL;
		goto out_put;
	}

	bo = to_nebulae_bo(obj);

	ret = nebulae_sync_all_bos_to_vram(ndev);
	if (ret)
		goto out_put;

	ret = nebulae_hw_submit_cmd_bo(ndev, bo, args->offset, args->size,
				       args->cmd_count, &seq, &hw_status);
	if (!ret)
		ret = nebulae_sync_all_bos_from_vram(ndev);
	args->seq = seq;
	args->status = hw_status;
	args->driver_error = ret;

out_put:
	drm_gem_object_put(obj);
	return ret;
}

static const struct drm_ioctl_desc nebulae_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NEBULAE_GET_PARAM, nebulae_ioctl_get_param,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_GET_INFO, nebulae_ioctl_get_info,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_BO_CREATE, nebulae_ioctl_bo_create,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_BO_MMAP, nebulae_ioctl_bo_mmap,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_BO_WAIT, nebulae_ioctl_bo_wait,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_SUBMIT, nebulae_ioctl_submit,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_MADVISE, nebulae_ioctl_madvise,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_SUBMIT_CMD_BO, nebulae_ioctl_submit_cmd_bo,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(nebulae_fops);

static const struct drm_driver nebulae_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
			   DRIVER_ATOMIC | DRIVER_SYNCOBJ |
			   DRIVER_SYNCOBJ_TIMELINE,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_create_object = nebulae_gem_create_object,
	.ioctls = nebulae_ioctls,
	.num_ioctls = ARRAY_SIZE(nebulae_ioctls),
	.fops = &nebulae_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRM_NEBULAE_DRIVER_MAJOR,
	.minor = DRM_NEBULAE_DRIVER_MINOR,
};

static int nebulae_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nebulae_device *ndev;
	struct drm_device *drm;
	struct resource *vram_res;
	u64 reported_vram;
	u32 magic;
	int ret;

	ndev = devm_drm_dev_alloc(dev, &nebulae_drm_driver,
				  struct nebulae_device, drm);
	if (IS_ERR(ndev))
		return PTR_ERR(ndev);

	ndev->pdev = pdev;
	drm = &ndev->drm;
	platform_set_drvdata(pdev, drm);

	ndev->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ndev->regs))
		return PTR_ERR(ndev->regs);

	ndev->vram = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(ndev->vram))
		return PTR_ERR(ndev->vram);

	magic = readl(ndev->regs + NEB_REG_MAGIC);
	if (magic != NEB_MAGIC)
		return dev_err_probe(dev, -ENODEV,
				     "bad Nebulae magic 0x%08x\n", magic);

	ndev->version = readl(ndev->regs + NEB_REG_VERSION);
	ndev->hw_caps = readl(ndev->regs + NEB_REG_CAPS);
	reported_vram = neb_readq(ndev, NEB_REG_VRAM_SIZE_LO);

	vram_res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	ndev->vram_size = resource_size(vram_res);
	if (reported_vram && reported_vram < ndev->vram_size)
		ndev->vram_size = reported_vram;
	if (ndev->vram_size <= NEB_SCANOUT_RESERVED + PAGE_SIZE)
		return -EINVAL;

	mutex_init(&ndev->bo_lock);
	mutex_init(&ndev->submit_lock);
	INIT_LIST_HEAD(&ndev->bo_list);
	ndev->vm_start = NEB_SCANOUT_RESERVED;
	ndev->vm_size = ndev->vram_size - ndev->vm_start;
	ndev->next_va = ndev->vm_start;
	WRITE_ONCE(ndev->last_error, readl(ndev->regs + NEB_REG_LAST_ERROR));
	atomic64_set(&ndev->submitted_jobs, 0);
	atomic64_set(&ndev->completed_jobs, 0);

	ndev->irq = platform_get_irq_optional(pdev, 0);
	if (ndev->irq > 0) {
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
		ret = devm_request_irq(dev, ndev->irq, nebulae_irq, 0,
				       dev_name(dev), ndev);
		if (ret)
			return ret;
	}

	ret = nebulae_kms_init(ndev);
	if (ret)
		return ret;

	ret = drm_dev_register(drm, 0);
	if (ret)
		return ret;

	drm_fbdev_shmem_setup(drm, 32);

	drm_info(drm,
		 "Nebulae DRM graphics v%08x caps 0x%08x vram %llu bytes vm [0x%llx-0x%llx) irq %d\n",
		 ndev->version, ndev->hw_caps, ndev->vram_size,
		 ndev->vm_start, ndev->vm_start + ndev->vm_size, ndev->irq);
	return 0;
}

static void nebulae_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct nebulae_device *ndev = to_nebulae(drm);

	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	if (ndev->regs)
		writel(0, ndev->regs + NEB_REG_IRQ_MASK);
}

static void nebulae_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (drm)
		drm_atomic_helper_shutdown(drm);
}

static const struct of_device_id nebulae_of_match[] = {
	{ .compatible = "nebulae,laxpu-simx-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, nebulae_of_match);

static struct platform_driver nebulae_platform_driver = {
	.probe = nebulae_probe,
	.remove = nebulae_remove,
	.shutdown = nebulae_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = nebulae_of_match,
	},
};
module_platform_driver(nebulae_platform_driver);

MODULE_AUTHOR("Nebulae");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
