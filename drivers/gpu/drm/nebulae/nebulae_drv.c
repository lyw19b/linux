// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae DRM graphics/display driver.
 *
 * This driver is independent from drivers/accel/nebulae.  It exposes the
 * Mesa-facing render/KMS device nodes while the accel driver remains dedicated
 * to accelerator compute workloads.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>

#include "nebulae_internal.h"

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

const struct drm_driver nebulae_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
			   DRIVER_ATOMIC | DRIVER_SYNCOBJ |
			   DRIVER_SYNCOBJ_TIMELINE,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.gem_create_object = nebulae_gem_create_object,
	.ioctls = nebulae_ioctls,
	.num_ioctls = ARRAY_SIZE(nebulae_ioctls),
	.fops = &nebulae_fops,
	.debugfs_init = nebulae_debugfs_init,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRM_NEBULAE_DRIVER_MAJOR,
	.minor = DRM_NEBULAE_DRIVER_MINOR,
};

static const struct of_device_id nebulae_of_match[] = {
	{ .compatible = "nebulae,laxpu-simx-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, nebulae_of_match);

static struct platform_driver nebulae_platform_driver = {
	.probe = nebulae_device_probe,
	.remove = nebulae_device_remove,
	.shutdown = nebulae_device_shutdown,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = nebulae_of_match,
	},
};
module_platform_driver(nebulae_platform_driver);

MODULE_AUTHOR("Nebulae");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
