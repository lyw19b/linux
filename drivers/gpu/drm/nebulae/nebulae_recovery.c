// SPDX-License-Identifier: GPL-2.0-only
/* Nebulae fault attribution, evidence capture and reset-domain recovery. */

#include <linux/devcoredump.h>
#include <linux/iopoll.h>
#include <linux/ktime.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include <drm/drm_modeset_helper.h>

#include "nebulae_internal.h"
#include "nebulae_trace.h"

struct nebulae_core_dump {
	u32 version;
	u32 reason;
	s32 error;
	u32 device_version;
	u32 hw_caps;
	u32 status;
	u32 irq_status;
	u32 last_error;
	u32 asid;
	u64 timestamp_ns;
	u64 ctx_id;
	u64 user_seq;
	u64 hw_seq;
	u64 pt_base;
	u64 submit_offset;
	u32 submit_size;
	u32 reserved;
};

struct nebulae_display_state {
	u32 enable;
	u32 width;
	u32 height;
	u32 stride;
	u32 format;
	u64 fb_base;
	u32 fb_size;
	u64 plane_base;
	u32 plane_stride;
	u32 plane_format;
	u32 plane_flags;
	u32 plane_size;
};

static void nebulae_save_display(struct nebulae_device *ndev,
				  struct nebulae_display_state *state)
{
	state->enable = readl(ndev->regs + NEB_REG_DISPLAY_ENABLE);
	state->width = readl(ndev->regs + NEB_REG_DISPLAY_WIDTH);
	state->height = readl(ndev->regs + NEB_REG_DISPLAY_HEIGHT);
	state->stride = readl(ndev->regs + NEB_REG_DISPLAY_STRIDE);
	state->format = readl(ndev->regs + NEB_REG_DISPLAY_FORMAT);
	state->fb_base = neb_readq(ndev, NEB_REG_DISPLAY_FB_BASE_LO);
	state->fb_size = readl(ndev->regs + NEB_REG_DISPLAY_FB_SIZE);
	state->plane_base = neb_readq(ndev, NEB_REG_DISPLAY_PLANE_BASE_LO);
	state->plane_stride = readl(ndev->regs + NEB_REG_DISPLAY_PLANE_STRIDE);
	state->plane_format = readl(ndev->regs + NEB_REG_DISPLAY_PLANE_FORMAT);
	state->plane_flags = readl(ndev->regs + NEB_REG_DISPLAY_PLANE_FLAGS);
	state->plane_size = readl(ndev->regs + NEB_REG_DISPLAY_PLANE_SIZE);
}

static void nebulae_restore_display(struct nebulae_device *ndev,
				     const struct nebulae_display_state *state)
{
	if (!state->enable)
		return;
	writel(state->width, ndev->regs + NEB_REG_DISPLAY_WIDTH);
	writel(state->height, ndev->regs + NEB_REG_DISPLAY_HEIGHT);
	writel(state->stride, ndev->regs + NEB_REG_DISPLAY_STRIDE);
	writel(state->format, ndev->regs + NEB_REG_DISPLAY_FORMAT);
	neb_writeq(ndev, NEB_REG_DISPLAY_FB_BASE_LO, state->fb_base);
	writel(state->fb_size, ndev->regs + NEB_REG_DISPLAY_FB_SIZE);
	neb_writeq(ndev, NEB_REG_DISPLAY_PLANE_BASE_LO, state->plane_base);
	writel(state->plane_stride,
	       ndev->regs + NEB_REG_DISPLAY_PLANE_STRIDE);
	writel(state->plane_format,
	       ndev->regs + NEB_REG_DISPLAY_PLANE_FORMAT);
	writel(state->plane_flags,
	       ndev->regs + NEB_REG_DISPLAY_PLANE_FLAGS);
	writel(state->plane_size, ndev->regs + NEB_REG_DISPLAY_PLANE_SIZE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_ENABLE);
	writel(1, ndev->regs + NEB_REG_DISPLAY_FLIP);
}

static void nebulae_capture_dump(struct nebulae_device *ndev,
				  struct nebulae_job *job, u32 reason,
				  int error)
{
	struct nebulae_core_dump *dump;

	/* dev_coredumpv() takes ownership and releases this with vfree(). */
	dump = vzalloc(sizeof(*dump));
	if (!dump)
		return;

	dump->version = 1;
	dump->reason = reason;
	dump->error = error;
	dump->device_version = ndev->version;
	dump->hw_caps = ndev->hw_caps;
	dump->status = readl(ndev->regs + NEB_REG_STATUS);
	dump->irq_status = readl(ndev->regs + NEB_REG_IRQ_STATUS);
	dump->last_error = readl(ndev->regs + NEB_REG_LAST_ERROR);
	dump->timestamp_ns = ktime_get_ns();
	dump->submit_offset = neb_readq(ndev, NEB_REG_SUBMIT_OFFSET_LO);
	dump->submit_size = readl(ndev->regs + NEB_REG_SUBMIT_SIZE);
	if (job) {
		dump->asid = job->asid;
		dump->ctx_id = job->nfile->ctx_id;
		dump->user_seq = job->user_seq;
		dump->hw_seq = job->hw_seq;
		dump->pt_base = job->pt_base;
	}

	dev_coredumpv(ndev->drm.dev, dump, sizeof(*dump), GFP_KERNEL);
}

void nebulae_fault_record(struct nebulae_device *ndev,
			  struct nebulae_job *job, u32 reason, u32 access,
			  u32 flags, u64 va, u32 hw_status, int error)
{
	struct drm_nebulae_fault fault = { };
	struct nebulae_file *nfile;
	unsigned long irqflags;

	if (!job || !job->nfile)
		return;
	nfile = job->nfile;

	fault.timestamp_ns = ktime_get_ns();
	fault.ctx_id = nfile->ctx_id;
	fault.job_seq = job->user_seq;
	fault.va = va;
	fault.asid = job->asid;
	fault.reason = reason;
	fault.access = access;
	fault.flags = flags;
	fault.hw_status = hw_status;
	fault.driver_error = error;

	spin_lock_irqsave(&nfile->fault_lock, irqflags);
	fault.sequence = ++nfile->fault_sequence;
	nfile->faults[nfile->fault_head] = fault;
	nfile->fault_head = (nfile->fault_head + 1) % NEB_FAULT_QUEUE_DEPTH;
	if (nfile->fault_count < NEB_FAULT_QUEUE_DEPTH)
		nfile->fault_count++;
	spin_unlock_irqrestore(&nfile->fault_lock, irqflags);

	spin_lock_irqsave(&ndev->fault_lock, irqflags);
	ndev->last_fault = fault;
	spin_unlock_irqrestore(&ndev->fault_lock, irqflags);
	trace_nebulae_fault(fault.ctx_id, fault.job_seq, fault.asid,
			     fault.reason, fault.flags, fault.va,
			     fault.hw_status, fault.driver_error);
}

int nebulae_ioctl_get_fault(struct drm_device *drm, void *data,
			    struct drm_file *file)
{
	struct drm_nebulae_get_fault *args = data;
	struct nebulae_file *nfile = file->driver_priv;
	unsigned long irqflags;
	u32 index;

	if (!nfile)
		return -ENODEV;
	if (args->pad || (args->flags & ~DRM_NEBULAE_GET_FAULT_PEEK))
		return -EINVAL;

	spin_lock_irqsave(&nfile->fault_lock, irqflags);
	if (!nfile->fault_count) {
		spin_unlock_irqrestore(&nfile->fault_lock, irqflags);
		return -ENODATA;
	}
	index = (nfile->fault_head + NEB_FAULT_QUEUE_DEPTH -
		 nfile->fault_count) % NEB_FAULT_QUEUE_DEPTH;
	args->fault = nfile->faults[index];
	if (!(args->flags & DRM_NEBULAE_GET_FAULT_PEEK))
		nfile->fault_count--;
	spin_unlock_irqrestore(&nfile->fault_lock, irqflags);

	return 0;
}

static int nebulae_save_vram_bos(struct nebulae_device *ndev)
{
	struct nebulae_bo *bo;
	int first_error = 0;
	int ret;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		if (bo->base.madv < 0)
			continue;
		if (!(bo->domain & DRM_NEBULAE_BO_DOMAIN_GPU))
			continue;
		ret = nebulae_bo_sync_from_vram_nowait(ndev, bo);
		if (ret && !first_error)
			first_error = ret;
	}
	mutex_unlock(&ndev->bo_lock);

	return first_error;
}

static int nebulae_restore_vram_bos(struct nebulae_device *ndev)
{
	struct nebulae_bo *bo;
	int ret = 0;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		if (bo->base.madv < 0)
			continue;
		ret = nebulae_bo_sync_to_vram_nowait(ndev, bo);
		if (ret)
			break;
	}
	mutex_unlock(&ndev->bo_lock);

	return ret;
}

static int nebulae_restore_contexts(struct nebulae_device *ndev)
{
	struct nebulae_file *nfile;
	int ret = 0;

	mutex_lock(&ndev->files_lock);
	list_for_each_entry(nfile, &ndev->files, device_link) {
		ret = nebulae_vm_restore(ndev, nfile);
		if (ret)
			break;
	}
	mutex_unlock(&ndev->files_lock);

	return ret;
}

int nebulae_device_reset(struct nebulae_device *ndev,
			 struct drm_sched_job *bad, u32 reason)
{
	struct nebulae_job *active;
	struct nebulae_display_state display;
	u32 status;
	int save_ret;
	int pm_ret;
	int ret;
	bool already_resetting;

	pm_ret = pm_runtime_resume_and_get(ndev->drm.dev);
	if (pm_ret < 0)
		return pm_ret;
	mutex_lock(&ndev->reset_lock);
	if (ndev->resetting || ndev->system_suspended || ndev->unplugged) {
		already_resetting = ndev->resetting;
		mutex_unlock(&ndev->reset_lock);
		pm_runtime_put(ndev->drm.dev);
		return already_resetting ? -EALREADY : -EHOSTDOWN;
	}
	ndev->resetting = true;
	trace_nebulae_reset("begin", reason,
			     atomic64_read(&ndev->reset_count), 0);

	/* Stop queue publication first, then quiesce all interrupt completion
	 * work before taking ownership of the active job and its shadow. */
	drm_sched_stop(&ndev->scheduler, bad);
	mutex_lock(&ndev->submit_lock);
	writel(0, ndev->regs + NEB_REG_IRQ_MASK);
	writel(0, ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	synchronize_irq(ndev->irq);
	cancel_work_sync(&ndev->irq_work);
	nebulae_save_display(ndev, &display);

	spin_lock_irq(&ndev->hw_lock);
	active = ndev->active_job;
	spin_unlock_irq(&ndev->hw_lock);
	nebulae_capture_dump(ndev, active, reason,
			      reason == DRM_NEBULAE_FAULT_TIMEOUT ?
			      -ETIMEDOUT : -EIO);
	nebulae_submit_abort_active(ndev,
				     reason == DRM_NEBULAE_FAULT_TIMEOUT ?
				     -ETIMEDOUT : -EIO, reason);
	wait_event(ndev->active_op_wait, !atomic_read(&ndev->active_ops));

	/* Preserve completed device writes in the sole authoritative shmem
	 * backing before the simulator reset clears its VRAM aperture. */
	save_ret = nebulae_save_vram_bos(ndev);
	if (save_ret)
		drm_warn(&ndev->drm,
			 "failed to preserve some VRAM data before reset: %d\n",
			 save_ret);

	writel(NEB_CONTROL_RESET, ndev->regs + NEB_REG_CONTROL);
	ret = readl_poll_timeout(ndev->regs + NEB_REG_STATUS, status,
				 !(status & NEB_STATUS_BUSY) &&
				 (status & NEB_STATUS_READY), 10, 1000000);
	if (!ret)
		ret = nebulae_restore_vram_bos(ndev);
	if (!ret)
		ret = nebulae_restore_contexts(ndev);
	if (!ret)
		nebulae_restore_display(ndev, &display);

	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	writel(NEB_DISPLAY_IRQ_FLIP_DONE,
	       ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);
	atomic_set(&ndev->pending_irq, 0);
	if (!ret && !ndev->suspended) {
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
		writel(NEB_DISPLAY_IRQ_FLIP_DONE,
		       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	}

	ndev->last_reset_reason = reason;
	ndev->last_reset_timestamp_ns = ktime_get_ns();
	atomic64_inc(&ndev->reset_count);
	ndev->wedged = ret != 0;
	ndev->resetting = false;
	mutex_unlock(&ndev->submit_lock);
	drm_sched_start(&ndev->scheduler, true);
	mutex_unlock(&ndev->reset_lock);
	trace_nebulae_reset("end", reason,
			     atomic64_read(&ndev->reset_count), ret);
	pm_runtime_mark_last_busy(ndev->drm.dev);
	pm_runtime_put_autosuspend(ndev->drm.dev);

	if (ret)
		drm_err(&ndev->drm, "reset/restore failed: %d; device wedged\n",
			ret);
	return ret;
}

int nebulae_runtime_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct nebulae_device *ndev;
	bool active;

	if (!drm)
		return 0;
	ndev = to_nebulae(drm);

	mutex_lock(&ndev->reset_lock);
	spin_lock_irq(&ndev->hw_lock);
	active = ndev->active_job != NULL;
	spin_unlock_irq(&ndev->hw_lock);
	if (ndev->resetting || active ||
	    atomic64_read(&ndev->running_jobs) ||
	    readl(ndev->regs + NEB_REG_DISPLAY_ENABLE)) {
		mutex_unlock(&ndev->reset_lock);
		return -EBUSY;
	}
	if (!ndev->system_suspended) {
		ndev->suspended = true;
		writel(0, ndev->regs + NEB_REG_IRQ_MASK);
		writel(0, ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
		synchronize_irq(ndev->irq);
	}
	mutex_unlock(&ndev->reset_lock);
	return 0;
}

int nebulae_runtime_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct nebulae_device *ndev;

	if (!drm)
		return 0;
	ndev = to_nebulae(drm);

	mutex_lock(&ndev->reset_lock);
	if (!ndev->system_suspended) {
		ndev->suspended = false;
		if (!ndev->wedged) {
			writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
			writel(NEB_DISPLAY_IRQ_FLIP_DONE,
			       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
		}
	}
	mutex_unlock(&ndev->reset_lock);
	return 0;
}

int nebulae_system_suspend(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct nebulae_device *ndev;
	int ret;

	if (!drm)
		return 0;
	ndev = to_nebulae(drm);
	ret = drm_mode_config_helper_suspend(drm);
	if (ret)
		return ret;

	mutex_lock(&ndev->reset_lock);
	ndev->system_suspended = true;
	ndev->suspended = true;
	drm_sched_stop(&ndev->scheduler, NULL);
	mutex_lock(&ndev->submit_lock);
	writel(0, ndev->regs + NEB_REG_IRQ_MASK);
	writel(0, ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	synchronize_irq(ndev->irq);
	cancel_work_sync(&ndev->irq_work);
	nebulae_submit_abort_active(ndev, -EHOSTDOWN,
				     DRM_NEBULAE_FAULT_RESET);
	wait_event(ndev->active_op_wait, !atomic_read(&ndev->active_ops));
	ret = nebulae_save_vram_bos(ndev);
	if (ret) {
		ndev->system_suspended = false;
		ndev->suspended = false;
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
		writel(NEB_DISPLAY_IRQ_FLIP_DONE,
		       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	}
	mutex_unlock(&ndev->submit_lock);
	if (ret)
		drm_sched_start(&ndev->scheduler, true);
	mutex_unlock(&ndev->reset_lock);

	if (ret)
		drm_mode_config_helper_resume(drm);
	return ret;
}

int nebulae_system_resume(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);
	struct nebulae_device *ndev;
	u32 status;
	int ret;

	if (!drm)
		return 0;
	ndev = to_nebulae(drm);

	mutex_lock(&ndev->reset_lock);
	mutex_lock(&ndev->submit_lock);
	writel(NEB_CONTROL_RESET, ndev->regs + NEB_REG_CONTROL);
	ret = readl_poll_timeout(ndev->regs + NEB_REG_STATUS, status,
				 !(status & NEB_STATUS_BUSY) &&
				 (status & NEB_STATUS_READY), 10, 1000000);
	if (!ret)
		ret = nebulae_restore_vram_bos(ndev);
	if (!ret)
		ret = nebulae_restore_contexts(ndev);
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	writel(NEB_DISPLAY_IRQ_FLIP_DONE,
	       ndev->regs + NEB_REG_DISPLAY_IRQ_STATUS);
	atomic_set(&ndev->pending_irq, 0);
	ndev->system_suspended = false;
	ndev->suspended = false;
	ndev->wedged = ret != 0;
	if (!ret) {
		writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_MASK);
		writel(NEB_DISPLAY_IRQ_FLIP_DONE,
		       ndev->regs + NEB_REG_DISPLAY_IRQ_MASK);
	}
	mutex_unlock(&ndev->submit_lock);
	drm_sched_start(&ndev->scheduler, true);
	mutex_unlock(&ndev->reset_lock);

	if (!ret)
		drm_mode_config_helper_resume(drm);
	return ret;
}

static void nebulae_reset_work(struct work_struct *work)
{
	struct nebulae_device *ndev =
		container_of(work, struct nebulae_device, reset_work);
	u32 reason = atomic_xchg(&ndev->reset_pending, 0);

	if (reason)
		nebulae_device_reset(ndev, NULL, reason);
}

void nebulae_schedule_reset(struct nebulae_device *ndev, u32 reason)
{
	if (READ_ONCE(ndev->system_suspended) || READ_ONCE(ndev->unplugged))
		return;
	if (!reason)
		reason = DRM_NEBULAE_FAULT_UNKNOWN;
	if (atomic_cmpxchg(&ndev->reset_pending, 0, reason) == 0)
		schedule_work(&ndev->reset_work);
}

void nebulae_recovery_init(struct nebulae_device *ndev)
{
	mutex_init(&ndev->reset_lock);
	spin_lock_init(&ndev->fault_lock);
	atomic64_set(&ndev->reset_count, 0);
	atomic_set(&ndev->reset_pending, 0);
	INIT_WORK(&ndev->reset_work, nebulae_reset_work);
}

void nebulae_recovery_fini(struct nebulae_device *ndev)
{
	cancel_work_sync(&ndev->reset_work);
}
