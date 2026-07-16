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
#include <linux/pm.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>

#include "nebulae_internal.h"

#define NEBULAE_GPU_PLATFORM_DRIVER_NAME	"nebulae-gpu"

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
	DRM_IOCTL_DEF_DRV(NEBULAE_BO_INFO, nebulae_ioctl_bo_info,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_BO_SET_DOMAIN, nebulae_ioctl_bo_set_domain,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_VM_BIND, nebulae_ioctl_vm_bind,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_JOB_CONTROL, nebulae_ioctl_job_control,
			  DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NEBULAE_GET_FAULT, nebulae_ioctl_get_fault,
			  DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(nebulae_fops);

const struct drm_driver nebulae_gpu_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_RENDER | DRIVER_MODESET |
			   DRIVER_ATOMIC | DRIVER_SYNCOBJ,
	.gem_prime_import_sg_table = drm_gem_shmem_prime_import_sg_table,
	.dumb_create = nebulae_dumb_create,
	.gem_prime_import = nebulae_gem_prime_import,
	.open = nebulae_file_open,
	.postclose = nebulae_file_postclose,
	.gem_create_object = nebulae_gpu_gem_create_object,
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
	{ .compatible = "nebulae,laxpu-gpu-simx-v1" },
	{ }
};
MODULE_DEVICE_TABLE(of, nebulae_of_match);

static const struct dev_pm_ops nebulae_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(nebulae_system_suspend,
				nebulae_system_resume)
	SET_RUNTIME_PM_OPS(nebulae_runtime_suspend,
			   nebulae_runtime_resume, NULL)
};

static struct platform_driver nebulae_platform_driver = {
	.probe = nebulae_device_probe,
	.remove = nebulae_device_remove,
	.shutdown = nebulae_device_shutdown,
	.driver = {
		.name = NEBULAE_GPU_PLATFORM_DRIVER_NAME,
		.of_match_table = nebulae_of_match,
		.pm = &nebulae_pm_ops,
	},
};
module_platform_driver(nebulae_platform_driver);

MODULE_AUTHOR("Nebulae");
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
