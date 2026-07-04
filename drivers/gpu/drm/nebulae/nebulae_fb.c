// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae framebuffer helpers.
 */

#include <linux/errno.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>

#include "nebulae_internal.h"

bool nebulae_fb_supported(const struct drm_framebuffer *fb)
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

struct drm_framebuffer *nebulae_fb_create(struct drm_device *drm,
					  struct drm_file *file,
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

	/* Install a .dirty callback (drm_atomic_helper_dirtyfb) so that a
	 * client's drmModeDirtyFB after rendering re-runs the plane atomic
	 * update -> nebulae_kms_update_scanout, re-blitting the framebuffer
	 * into the scanout VRAM.  Without this the shadow blit only happens on
	 * modeset/pageflip, so continuous rendering (e.g. glxgears via the
	 * modesetting CopyArea/dirty path) never reaches the display. */
	return drm_gem_fb_create_with_dirty(drm, file, mode_cmd);
}
