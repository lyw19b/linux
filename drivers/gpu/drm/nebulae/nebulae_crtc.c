// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae simple CRTC/display pipe setup.
 */

#include <drm/drm_fourcc.h>
#include <drm/drm_simple_kms_helper.h>

#include "nebulae_internal.h"

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

static const struct drm_simple_display_pipe_funcs nebulae_pipe_funcs = {
	.mode_valid = nebulae_pipe_mode_valid,
	.enable = nebulae_plane_enable,
	.disable = nebulae_plane_disable,
	.check = nebulae_plane_check,
	.update = nebulae_plane_update,
	.enable_vblank = nebulae_vblank_enable,
	.disable_vblank = nebulae_vblank_disable,
};

static const u32 nebulae_pipe_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const u64 nebulae_pipe_modifiers[] = {
	DRM_FORMAT_MOD_LINEAR,
	DRM_FORMAT_MOD_INVALID,
};

int nebulae_crtc_init(struct nebulae_device *ndev)
{
	return drm_simple_display_pipe_init(&ndev->drm, &ndev->pipe,
					    &nebulae_pipe_funcs,
					    nebulae_pipe_formats,
					    ARRAY_SIZE(nebulae_pipe_formats),
					    nebulae_pipe_modifiers,
					    &ndev->connector);
}
