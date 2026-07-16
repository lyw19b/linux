// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae primary plane update path.
 */

#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>

#include <drm/drm_drv.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_rect.h>
#include <drm/drm_vblank.h>

#include "nebulae_internal.h"

u64 nebulae_scanout_size(unsigned int width, unsigned int height,
			 unsigned int pitch)
{
	if (!width || !height)
		return 0;

	return (u64)(height - 1) * pitch + (u64)width * 4;
}

static void nebulae_program_display(struct nebulae_device *ndev,
				    unsigned int width, unsigned int height,
				    unsigned int pitch, u64 scanout_size,
				    u64 plane_base, unsigned int plane_pitch,
				    u32 plane_format, u32 plane_flags,
				    u64 plane_size)
{
	unsigned long flags;
	u64 active_base = plane_flags & NEB_DISPLAY_PLANE_VALID ?
			  plane_base : 0;

	/* Publish ownership before the MMIO tuple.  A concurrent submit may be
	 * classified conservatively against the new plane, while atomic FB
	 * reservation fences prevent that plane becoming active ahead of a writer
	 * which already references it. */
	spin_lock_irqsave(&ndev->scanout_lock, flags);
	ndev->scanout_base = active_base;
	ndev->scanout_size = scanout_size;
	ndev->scanout_direct =
		(plane_flags & NEB_DISPLAY_PLANE_VALID) != 0;
	ndev->scanout_generation++;

	writel(0, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	writel(width, ndev->regs + NEB_REG_DISPLAY_WIDTH);
	writel(height, ndev->regs + NEB_REG_DISPLAY_HEIGHT);
	writel(pitch, ndev->regs + NEB_REG_DISPLAY_STRIDE);
	writel(NEB_DISPLAY_FORMAT_XRGB8888,
	       ndev->regs + NEB_REG_DISPLAY_FORMAT);
	neb_writeq(ndev, NEB_REG_DISPLAY_FB_BASE_LO, 0);
	writel((u32)scanout_size, ndev->regs + NEB_REG_DISPLAY_FB_SIZE);
	neb_writeq(ndev, NEB_REG_DISPLAY_PLANE_BASE_LO, plane_base);
	writel(plane_pitch, ndev->regs + NEB_REG_DISPLAY_PLANE_STRIDE);
	writel(plane_format, ndev->regs + NEB_REG_DISPLAY_PLANE_FORMAT);
	writel(plane_flags, ndev->regs + NEB_REG_DISPLAY_PLANE_FLAGS);
	writel((u32)plane_size, ndev->regs + NEB_REG_DISPLAY_PLANE_SIZE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_FLIP);
	spin_unlock_irqrestore(&ndev->scanout_lock, flags);
	nebulae_vblank_record_flip(ndev);
}

static bool nebulae_fb_has_gpu_plane(struct nebulae_bo *bo, u64 offset,
				     u64 scanout_size)
{
	struct drm_gem_object *obj = &bo->base.base;

	return bo->vram_offset && offset <= obj->size &&
	       scanout_size <= obj->size - offset;
}

static u32 nebulae_fb_plane_format(const struct drm_framebuffer *fb)
{
	switch (fb->format->format) {
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XRGB8888:
	default:
		return NEB_DISPLAY_FORMAT_XRGB8888;
	}
}

static void nebulae_program_gpu_plane(struct nebulae_device *ndev,
				      struct drm_framebuffer *fb,
				      struct nebulae_bo *bo,
				      unsigned int width, unsigned int height,
				      u64 scanout_size)
{
	u64 offset = fb->offsets[0];

	mutex_lock(&ndev->bo_lock);
	bo->domain |= DRM_NEBULAE_BO_DOMAIN_SCANOUT;
	mutex_unlock(&ndev->bo_lock);

	nebulae_program_display(ndev, width, height, fb->pitches[0],
				 scanout_size, bo->vram_offset + offset, fb->pitches[0],
				 nebulae_fb_plane_format(fb),
				 NEB_DISPLAY_PLANE_VALID, scanout_size);
}

static void nebulae_kms_update_scanout(struct nebulae_device *ndev,
				       struct drm_plane_state *plane_state)
{
	struct drm_framebuffer *fb;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
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
	obj = drm_gem_fb_get_obj(fb, 0);
	if (!obj)
		return;
	bo = to_nebulae_bo(obj);
	width = plane_state->crtc_w ?: fb->width;
	height = plane_state->crtc_h ?: fb->height;
	scanout_size = nebulae_scanout_size(width, height, fb->pitches[0]);
	if (!scanout_size || scanout_size > ndev->vram_size)
		return;

	if (nebulae_fb_has_gpu_plane(bo, fb->offsets[0], scanout_size)) {
		ret = nebulae_device_enter(ndev, &idx);
		if (ret)
			return;
		if (bo->domain & DRM_NEBULAE_BO_DOMAIN_CPU) {
			ret = nebulae_bo_sync_to_vram(ndev, bo);
			if (ret) {
				nebulae_device_exit(ndev, idx);
				return;
			}
		}
		nebulae_program_gpu_plane(ndev, fb, bo, width, height,
					   scanout_size);
		nebulae_device_exit(ndev, idx);
		return;
	}

	/* Atomic GEM helpers resolve framebuffer dependencies before this update.
	 * Do not introduce a second reservation wait while mapping the fallback
	 * shadow-blit source. */
	ret = drm_gem_fb_vmap(fb, map, data);
	if (ret)
		return;
	if (data[0].is_iomem)
		goto out_vunmap;

	ret = nebulae_device_enter(ndev, &idx);
	if (ret)
		goto out_vunmap;

	dst_pitch[0] = fb->pitches[0];
	clip = DRM_RECT_INIT(0, 0, width, height);
	drm_fb_memcpy(&dst, dst_pitch, data, fb, &clip);
	nebulae_program_display(ndev, width, height, fb->pitches[0],
				 scanout_size, 0, 0,
				 NEB_DISPLAY_FORMAT_XRGB8888, 0, 0);

	nebulae_device_exit(ndev, idx);

out_vunmap:
	drm_gem_fb_vunmap(fb, map);
}

static void nebulae_pipe_send_event(struct drm_simple_display_pipe *pipe)
{
	struct drm_crtc *crtc = &pipe->crtc;
	struct drm_pending_vblank_event *event = crtc->state->event;

	if (!event)
		return;

	crtc->state->event = NULL;

	spin_lock_irq(&crtc->dev->event_lock);
	if (crtc->state->active && drm_crtc_vblank_get(crtc) == 0)
		drm_crtc_arm_vblank_event(crtc, event);
	else
		drm_crtc_send_vblank_event(crtc, event);
	spin_unlock_irq(&crtc->dev->event_lock);
}

int nebulae_plane_check(struct drm_simple_display_pipe *pipe,
			struct drm_plane_state *plane_state,
			struct drm_crtc_state *crtc_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	struct drm_framebuffer *fb = plane_state->fb;
	struct drm_gem_object *obj;
	unsigned int width;
	unsigned int height;
	u64 scanout_size;

	crtc_state->no_vblank = false;

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

void nebulae_plane_enable(struct drm_simple_display_pipe *pipe,
			  struct drm_crtc_state *crtc_state,
			  struct drm_plane_state *plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	int ret;

	if (!ndev->display_pm_ref) {
		ret = pm_runtime_resume_and_get(ndev->drm.dev);
		if (ret < 0)
			return;
		ndev->display_pm_ref = true;
	}

	nebulae_kms_update_scanout(ndev, plane_state);
	drm_crtc_vblank_on(&pipe->crtc);
	nebulae_pipe_send_event(pipe);
}

void nebulae_plane_disable(struct drm_simple_display_pipe *pipe)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	unsigned long flags;
	int idx;
	int ret;

	drm_crtc_vblank_off(&pipe->crtc);
	ret = nebulae_device_enter(ndev, &idx);
	if (!ret) {
		spin_lock_irqsave(&ndev->scanout_lock, flags);
		writel(0, ndev->regs + NEB_REG_DISPLAY_ENABLE);
		ndev->scanout_base = 0;
		ndev->scanout_size = 0;
		ndev->scanout_direct = false;
		ndev->scanout_generation++;
		spin_unlock_irqrestore(&ndev->scanout_lock, flags);
		nebulae_device_exit(ndev, idx);
	}
	if (ndev->display_pm_ref) {
		ndev->display_pm_ref = false;
		pm_runtime_mark_last_busy(ndev->drm.dev);
		pm_runtime_put_autosuspend(ndev->drm.dev);
	}
}

void nebulae_plane_update(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *old_plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	nebulae_kms_update_scanout(ndev, pipe->plane.state);
	nebulae_pipe_send_event(pipe);
}
