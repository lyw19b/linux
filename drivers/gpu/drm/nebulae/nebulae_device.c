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
#include <linux/pm_runtime.h>
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

int nebulae_device_enter(struct nebulae_device *ndev, int *idx)
{
	int ret;

	ret = pm_runtime_resume_and_get(ndev->drm.dev);
	if (ret < 0)
		return ret;
	if (!drm_dev_enter(&ndev->drm, idx)) {
		ret = -ENODEV;
		goto err_pm;
	}
	mutex_lock(&ndev->reset_lock);
	if (ndev->resetting) {
		ret = -EAGAIN;
		goto err_unlock;
	}
	if (ndev->suspended || ndev->wedged || ndev->unplugged) {
		ret = -ENODEV;
		goto err_unlock;
	}
	atomic_inc(&ndev->active_ops);
	mutex_unlock(&ndev->reset_lock);
	return 0;

err_unlock:
	mutex_unlock(&ndev->reset_lock);
	drm_dev_exit(*idx);
err_pm:
	pm_runtime_mark_last_busy(ndev->drm.dev);
	pm_runtime_put_autosuspend(ndev->drm.dev);
	return ret;
}

void nebulae_device_exit(struct nebulae_device *ndev, int idx)
{
	if (atomic_dec_and_test(&ndev->active_ops))
		wake_up_all(&ndev->active_op_wait);
	drm_dev_exit(idx);
	pm_runtime_mark_last_busy(ndev->drm.dev);
	pm_runtime_put_autosuspend(ndev->drm.dev);
}

struct nebulae_hw_profile {
	u32 version;
	u32 required_caps;
	struct drm_nebulae_device_info info;
};

/* NEB_REG_VERSION is the v1 simulator's read-only discovery key.  Resource
 * limits are deliberately limited to what the in-kernel v5.7 validator
 * understands, rather than copying larger aspirational schema maxima. */
static const struct nebulae_hw_profile nebulae_hw_profiles[] = {
	{
		.version = 0x00010000,
		.required_caps = NEB_CAP_IRQ | NEB_CAP_CP_QUEUE |
				 NEB_CAP_TLB_INVALIDATE | NEB_CAP_FULL_GPUVM |
				 NEB_CAP_ICACHE_INVALIDATE,
		.info = {
			.isa_major = 5,
			.isa_minor = 7,
			.wave_size = 32,
			.num_compute_units = 1,
			.max_sr_count = 100,
			.max_user_sr_count = 80,
			.max_vr_count = 128,
			.max_scratch_bytes_per_wave = 512,
			.max_wgm_bytes_per_workgroup = 32 * 1024,
			.max_waves_per_cu = 16,
			.max_workgroup_invocations = 1024,
			.max_textures = 256,
			.max_samplers = 256,
			.max_images = 8,
			.max_ubos = 256,
			.max_ssbos = 256,
		},
	},
};

static int nebulae_discover_device(struct nebulae_device *ndev)
{
	const struct nebulae_hw_profile *profile = NULL;
	u64 features = NEB_FEATURE_CORE | NEB_FEATURE_MEMORY | NEB_FEATURE_MMU;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nebulae_hw_profiles); i++) {
		if (nebulae_hw_profiles[i].version == ndev->version) {
			profile = &nebulae_hw_profiles[i];
			break;
		}
	}
	if (!profile)
		return dev_err_probe(ndev->drm.dev, -ENODEV,
				     "unknown discovery version 0x%08x\n",
				     ndev->version);
	if ((ndev->hw_caps & profile->required_caps) != profile->required_caps)
		return dev_err_probe(ndev->drm.dev, -ENODEV,
				     "version 0x%08x missing required caps 0x%08x (got 0x%08x)\n",
				     ndev->version, profile->required_caps,
				     ndev->hw_caps);

	ndev->device_info = profile->info;
	ndev->device_info.vram_size = ndev->vram_size;
	if (ndev->hw_caps & NEB_CAP_CP_QUEUE)
		features |= NEB_FEATURE_GRAPHICS | NEB_FEATURE_TEXTURE;
	if (ndev->hw_caps & NEB_CAP_NDRANGE)
		features |= NEB_FEATURE_COMPUTE;
	ndev->device_info.supported_features = features & NEB_SUPPORTED_FEATURES;
	return 0;
}

void nebulae_fill_device_info(struct nebulae_device *ndev,
			      struct drm_nebulae_device_info *info)
{
	*info = ndev->device_info;
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
			 DRM_NEBULAE_SUBMIT_CAP_FENCE_FD |
			 DRM_NEBULAE_SUBMIT_CAP_BO_LIST;
		if (ndev->hw_caps & NEB_CAP_JOB_CONTROL)
			*value |= DRM_NEBULAE_SUBMIT_CAP_JOB_CONTROL;
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
	ret = nebulae_discover_device(ndev);
	if (ret)
		return ret;

	mutex_init(&ndev->bo_lock);
	mutex_init(&ndev->submit_lock);
	mutex_init(&ndev->files_lock);
	spin_lock_init(&ndev->scanout_lock);
	init_waitqueue_head(&ndev->submit_wait);
	init_waitqueue_head(&ndev->active_op_wait);
	atomic_set(&ndev->active_ops, 0);
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
	spin_lock_init(&ndev->hw_lock);
	nebulae_irq_init(ndev);
	nebulae_recovery_init(ndev);
	ndev->irq = -1;

	/* Render completion is a real IRQ fence.  Do not expose a pseudo-async
	 * driver on platforms that omitted the completion interrupt. */
	ndev->irq = platform_get_irq(pdev, 0);
	if (ndev->irq < 0) {
		ret = ndev->irq;
		goto err_irq;
	}
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	writel(NEB_DISPLAY_IRQ_FLIP_DONE,
	       ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);
	ret = devm_request_irq(dev, ndev->irq, nebulae_gpu_irq, 0,
			       dev_name(dev), ndev);
	if (ret)
		goto err_irq;
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
	writel(NEB_DISPLAY_IRQ_FLIP_DONE,
	       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);

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
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_idle(dev);

	drm_info(drm,
		 "Nebulae DRM graphics v%08x caps 0x%08x vram %llu bytes vm [0x%llx-0x%llx) irq %d\n",
		 ndev->version, ndev->hw_caps, ndev->vram_size,
		 ndev->vm_start, ndev->vm_start + ndev->vm_size, ndev->irq);
	return 0;

err_unregister:
	drm_dev_unregister(drm);
	nebulae_vblank_fini(ndev);
err_sched:
	if (ndev->irq >= 0) {
		writel(0, ndev->regs + NEB_REG_IRQ_MASK);
		synchronize_irq(ndev->irq);
	}
err_irq:
	nebulae_recovery_fini(ndev);
	nebulae_irq_fini(ndev);
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
	struct device *dev = &pdev->dev;

	nebulae_gpu_sysfs_fini(ndev);
	drm_atomic_helper_shutdown(drm);
	nebulae_vblank_fini(ndev);
	pm_runtime_get_sync(dev);
	pm_runtime_disable(dev);
	pm_runtime_put_noidle(dev);
	/* Block all new drm_dev_enter() users before platform resources can be
	 * released, then drain recovery/IRQ completion and active DMA state. */
	mutex_lock(&ndev->reset_lock);
	WRITE_ONCE(ndev->unplugged, true);
	WRITE_ONCE(ndev->suspended, true);
	WRITE_ONCE(ndev->wedged, true);
	mutex_unlock(&ndev->reset_lock);
	drm_dev_unplug(drm);
	if (ndev->regs) {
		writel(0, ndev->regs + NEB_REG_IRQ_MASK);
		writel(0, ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	}
	if (ndev->irq >= 0)
		synchronize_irq(ndev->irq);
	nebulae_irq_fini(ndev);
	nebulae_recovery_fini(ndev);
	nebulae_submit_abort_active(ndev, -ENODEV, DRM_NEBULAE_FAULT_RESET);
	wait_event(ndev->active_op_wait, !atomic_read(&ndev->active_ops));
	nebulae_gpu_sched_fini(ndev);
	nebulae_ctx_fini(ndev);
	nebulae_mmu_fini(ndev);
	nebulae_vm_fini(ndev);
}

void nebulae_device_shutdown(struct platform_device *pdev)
{
	struct drm_device *drm = platform_get_drvdata(pdev);

	if (drm) {
		struct nebulae_device *ndev = to_nebulae(drm);

		drm_atomic_helper_shutdown(drm);
		nebulae_vblank_fini(ndev);
	}
}
