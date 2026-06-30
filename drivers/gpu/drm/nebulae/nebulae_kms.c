// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae KMS mode configuration.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_managed.h>

#include "nebulae_internal.h"

static const struct drm_mode_config_funcs nebulae_mode_config_funcs = {
	.fb_create = nebulae_fb_create,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

int nebulae_kms_init(struct nebulae_device *ndev)
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

	ret = nebulae_output_init(ndev);
	if (ret)
		return ret;

	ret = nebulae_crtc_init(ndev);
	if (ret)
		return ret;

	ret = nebulae_vblank_init(ndev);
	if (ret)
		return ret;

	drm_mode_config_reset(drm);
	return 0;
}
