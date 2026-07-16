// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae virtual output connector.
 */

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>

#include "nebulae_internal.h"

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

static enum drm_connector_status
nebulae_connector_detect(struct drm_connector *connector, bool force)
{
	/* This is an explicitly virtual, permanently attached output.  There is
	 * no EDID or hotplug source in the v1 simulator. */
	return connector_status_connected;
}

static const struct drm_connector_funcs nebulae_connector_funcs = {
	.detect = nebulae_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

int nebulae_output_init(struct nebulae_device *ndev)
{
	struct drm_device *drm = &ndev->drm;
	int ret;

	ret = drmm_connector_init(drm, &ndev->connector,
				  &nebulae_connector_funcs,
				  DRM_MODE_CONNECTOR_VIRTUAL, NULL);
	if (ret)
		return ret;

	ndev->connector.polled = 0;
	drm_connector_helper_add(&ndev->connector,
				 &nebulae_connector_helper_funcs);
	return 0;
}
