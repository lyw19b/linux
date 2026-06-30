// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae primary plane update path.
 */

#include <linux/dma-direction.h>
#include <linux/errno.h>
#include <linux/io.h>

#include <drm/drm_drv.h>
#include <drm/drm_format_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_atomic_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_rect.h>

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
	nebulae_vblank_record_flip(ndev);
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

void nebulae_plane_enable(struct drm_simple_display_pipe *pipe,
			  struct drm_crtc_state *crtc_state,
			  struct drm_plane_state *plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	nebulae_kms_update_scanout(ndev, plane_state);
}

void nebulae_plane_disable(struct drm_simple_display_pipe *pipe)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);
	int idx;

	if (!drm_dev_enter(&ndev->drm, &idx))
		return;

	writel(0, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	drm_dev_exit(idx);
}

void nebulae_plane_update(struct drm_simple_display_pipe *pipe,
			  struct drm_plane_state *old_plane_state)
{
	struct nebulae_device *ndev = to_nebulae(pipe->crtc.dev);

	nebulae_kms_update_scanout(ndev, pipe->plane.state);
}
