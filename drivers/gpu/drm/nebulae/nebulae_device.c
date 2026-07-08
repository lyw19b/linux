// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae DRM device probing and static device info.
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/string.h>

#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_shmem.h>
#include <drm/drm_print.h>

#include "nebulae_internal.h"

#define NEB_FEATURE_CORE	BIT_ULL(0)
#define NEB_FEATURE_GRAPHICS	BIT_ULL(1)
#define NEB_FEATURE_COMPUTE	BIT_ULL(2)
#define NEB_FEATURE_MEMORY	BIT_ULL(3)
#define NEB_FEATURE_TEXTURE	BIT_ULL(4)
#define NEB_FEATURE_MMU	BIT_ULL(15)
#define NEB_SUPPORTED_FEATURES	(NEB_FEATURE_CORE | NEB_FEATURE_GRAPHICS | \
				 NEB_FEATURE_COMPUTE | NEB_FEATURE_MEMORY | \
				 NEB_FEATURE_TEXTURE | NEB_FEATURE_MMU)

void nebulae_fill_device_info(struct nebulae_device *ndev,
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

int nebulae_get_param_value(struct nebulae_device *ndev, u32 param,
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
		*value = DRM_NEBULAE_SUBMIT_CAP_ASYNC |
			 DRM_NEBULAE_SUBMIT_CAP_CMD_BO |
			 DRM_NEBULAE_SUBMIT_CAP_SYNCOBJ |
			 DRM_NEBULAE_SUBMIT_CAP_FENCE_FD;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

int nebulae_ioctl_get_param(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_get_param *args = data;

	if (args->pad)
		return -EINVAL;

	return nebulae_get_param_value(ndev, args->param, &args->value);
}

int nebulae_ioctl_get_info(struct drm_device *drm, void *data,
			   struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_nebulae_device_info *args = data;

	nebulae_fill_device_info(ndev, args);
	return 0;
}

int nebulae_device_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct nebulae_device *ndev;
	struct drm_device *drm;
	struct resource *vram_res;
	u64 reported_vram;
	u32 magic;
	int ret;

	ndev = devm_drm_dev_alloc(dev, &nebulae_gpu_drm_driver,
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
	ndev->vram_phys = vram_res->start;
	ndev->vram_size = resource_size(vram_res);
	if (reported_vram && reported_vram < ndev->vram_size)
		ndev->vram_size = reported_vram;

	mutex_init(&ndev->bo_lock);
	mutex_init(&ndev->submit_lock);
	init_waitqueue_head(&ndev->submit_wait);
	INIT_LIST_HEAD(&ndev->bo_list);
	ret = nebulae_vm_init(ndev);
	if (ret)
		return ret;

	ret = nebulae_mmu_init(ndev);
	if (ret)
		goto err_vm;

	WRITE_ONCE(ndev->last_error, readl(ndev->regs + NEB_REG_LAST_ERROR));
	atomic64_set(&ndev->submitted_jobs, 0);
	atomic64_set(&ndev->completed_jobs, 0);
	atomic64_set(&ndev->irq_count, 0);
	atomic64_set(&ndev->complete_irq_count, 0);
	atomic64_set(&ndev->fault_irq_count, 0);
	atomic64_set(&ndev->display_irq_count, 0);
	ndev->last_submit_cookie = 0;
	ndev->last_submit_pt_base = 0;
	ndev->last_submit_asid = 0;
	ndev->last_irq_status = 0;
	ndev->last_display_irq_status = 0;

	ret = nebulae_ctx_init(ndev);
	if (ret)
		goto err_vm;

	ret = nebulae_sched_init(ndev);
	if (ret)
		goto err_ctx;

	ndev->irq = platform_get_irq_optional(pdev, 0);
	if (ndev->irq > 0) {
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
		writel(NEB_DISPLAY_IRQ_FLIP_DONE,
		       ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
		writel(NEB_DISPLAY_IRQ_FLIP_DONE,
		       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
		ret = devm_request_irq(dev, ndev->irq, nebulae_gpu_irq, 0,
				       dev_name(dev), ndev);
		if (ret)
			goto err_sched;
	}

	ret = nebulae_kms_init(ndev);
	if (ret)
		goto err_sched;

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_sched;

	ret = nebulae_gpu_sysfs_init(ndev);
	if (ret)
		goto err_unregister;

	drm_fbdev_shmem_setup(drm, 32);

	drm_info(drm,
		 "Nebulae DRM graphics v%08x caps 0x%08x vram %llu bytes vm [0x%llx-0x%llx) irq %d\n",
		 ndev->version, ndev->hw_caps, ndev->vram_size,
		 ndev->vm_start, ndev->vm_start + ndev->vm_size, ndev->irq);
	return 0;

err_unregister:
	drm_dev_unregister(drm);
err_sched:
	nebulae_gpu_sched_fini(ndev);
err_ctx:
	nebulae_ctx_fini(ndev);
err_vm:
	nebulae_mmu_fini(ndev);
	nebulae_vm_fini(ndev);
	return ret;
}

void nebulae_device_remove(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);
	struct nebulae_device *ndev = to_nebulae(drm);

	nebulae_gpu_sysfs_fini(ndev);
	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	if (ndev->regs) {
		writel(0, ndev->regs + NEB_REG_IRQ_MASK);
		writel(0, ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	}
	nebulae_gpu_sched_fini(ndev);
	nebulae_ctx_fini(ndev);
	nebulae_mmu_fini(ndev);
	nebulae_vm_fini(ndev);
}

void nebulae_device_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (drm)
		drm_atomic_helper_shutdown(drm);
}
