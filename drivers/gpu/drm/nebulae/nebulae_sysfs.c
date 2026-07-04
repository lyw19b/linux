// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae sysfs hooks.
 */

#include <linux/device.h>
#include <linux/sysfs.h>

#include "nebulae_internal.h"

static struct nebulae_device *nebulae_sysfs_device(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	return to_nebulae(drm);
}

static ssize_t vram_size_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%llu\n", ndev->vram_size);
}
static DEVICE_ATTR_RO(vram_size);

static ssize_t vm_start_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "0x%llx\n", ndev->vm_start);
}
static DEVICE_ATTR_RO(vm_start);

static ssize_t vm_size_show(struct device *dev,
			    struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "0x%llx\n", ndev->vm_size);
}
static DEVICE_ATTR_RO(vm_size);

static ssize_t submitted_jobs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->submitted_jobs));
}
static DEVICE_ATTR_RO(submitted_jobs);

static ssize_t completed_jobs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->completed_jobs));
}
static DEVICE_ATTR_RO(completed_jobs);

static ssize_t scheduled_jobs_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->scheduled_jobs));
}
static DEVICE_ATTR_RO(scheduled_jobs);

static ssize_t running_jobs_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&ndev->running_jobs));
}
static DEVICE_ATTR_RO(running_jobs);

static ssize_t finished_jobs_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->finished_jobs));
}
static DEVICE_ATTR_RO(finished_jobs);

static ssize_t failed_jobs_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&ndev->failed_jobs));
}
static DEVICE_ATTR_RO(failed_jobs);

static ssize_t signaled_fences_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->signaled_fences));
}
static DEVICE_ATTR_RO(signaled_fences);

static ssize_t irq_count_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n", atomic64_read(&ndev->irq_count));
}
static DEVICE_ATTR_RO(irq_count);

static ssize_t display_flips_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "%lld\n",
			  atomic64_read(&ndev->display_flips));
}
static DEVICE_ATTR_RO(display_flips);

static ssize_t last_error_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct nebulae_device *ndev = nebulae_sysfs_device(dev);

	return sysfs_emit(buf, "0x%08x\n", READ_ONCE(ndev->last_error));
}
static DEVICE_ATTR_RO(last_error);

static struct attribute *nebulae_sysfs_attrs[] = {
	&dev_attr_vram_size.attr,
	&dev_attr_vm_start.attr,
	&dev_attr_vm_size.attr,
	&dev_attr_submitted_jobs.attr,
	&dev_attr_completed_jobs.attr,
	&dev_attr_scheduled_jobs.attr,
	&dev_attr_running_jobs.attr,
	&dev_attr_finished_jobs.attr,
	&dev_attr_failed_jobs.attr,
	&dev_attr_signaled_fences.attr,
	&dev_attr_irq_count.attr,
	&dev_attr_display_flips.attr,
	&dev_attr_last_error.attr,
	NULL,
};

static const struct attribute_group nebulae_sysfs_attr_group = {
	.name = "nebulae",
	.attrs = nebulae_sysfs_attrs,
};

int nebulae_gpu_sysfs_init(struct nebulae_device *ndev)
{
	int ret;

	ret = sysfs_create_group(&ndev->drm.dev->kobj,
				 &nebulae_sysfs_attr_group);
	if (ret)
		return ret;

	ndev->sysfs_registered = true;
	return 0;
}

void nebulae_gpu_sysfs_fini(struct nebulae_device *ndev)
{
	if (!ndev->sysfs_registered)
		return;

	sysfs_remove_group(&ndev->drm.dev->kobj, &nebulae_sysfs_attr_group);
	ndev->sysfs_registered = false;
}
