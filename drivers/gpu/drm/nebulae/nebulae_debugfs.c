// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae debugfs hooks.
 */

#include <drm/drm_debugfs.h>
#include <drm/drm_print.h>

#include "nebulae_internal.h"

struct nebulae_debugfs_reg {
	u32 reg;
	const char *name;
};

#define NEB_DEBUGFS_REG(_reg) { _reg, #_reg }

static const struct nebulae_debugfs_reg nebulae_debugfs_regs[] = {
	NEB_DEBUGFS_REG(NEB_REG_MAGIC),
	NEB_DEBUGFS_REG(NEB_REG_VERSION),
	NEB_DEBUGFS_REG(NEB_REG_CAPS),
	NEB_DEBUGFS_REG(NEB_REG_CONTROL),
	NEB_DEBUGFS_REG(NEB_REG_STATUS),
	NEB_DEBUGFS_REG(NEB_REG_IRQ_STATUS),
	NEB_DEBUGFS_REG(NEB_REG_IRQ_MASK),
	NEB_DEBUGFS_REG(NEB_REG_VRAM_SIZE_LO),
	NEB_DEBUGFS_REG(NEB_REG_VRAM_SIZE_HI),
	NEB_DEBUGFS_REG(NEB_REG_SUBMIT_OFFSET_LO),
	NEB_DEBUGFS_REG(NEB_REG_SUBMIT_OFFSET_HI),
	NEB_DEBUGFS_REG(NEB_REG_SUBMIT_SIZE),
	NEB_DEBUGFS_REG(NEB_REG_DOORBELL),
	NEB_DEBUGFS_REG(NEB_REG_JOB_SEQ_LO),
	NEB_DEBUGFS_REG(NEB_REG_JOB_SEQ_HI),
	NEB_DEBUGFS_REG(NEB_REG_COMPLETED_SEQ_LO),
	NEB_DEBUGFS_REG(NEB_REG_COMPLETED_SEQ_HI),
	NEB_DEBUGFS_REG(NEB_REG_LAST_ERROR),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_ENABLE),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_WIDTH),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_HEIGHT),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_STRIDE),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_FORMAT),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_FB_BASE_LO),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_FB_BASE_HI),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_FB_SIZE),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_FLIP),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_IRQ_STATUS),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_IRQ_MASK),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_BASE_LO),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_BASE_HI),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_STRIDE),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_FORMAT),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_FLAGS),
	NEB_DEBUGFS_REG(NEB_REG_DISPLAY_PLANE_SIZE),
};

static struct nebulae_device *nebulae_debugfs_device(struct seq_file *m)
{
	struct drm_info_node *node = m->private;

	return to_nebulae(node->minor->dev);
}

static int nebulae_debugfs_info(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);

	seq_printf(m, "version: 0x%08x\n", ndev->version);
	seq_printf(m, "hw_caps: 0x%08x\n", ndev->hw_caps);
	seq_printf(m, "vram_size: %llu\n", ndev->vram_size);
	seq_printf(m, "vm_start: 0x%llx\n", ndev->vm_start);
	seq_printf(m, "vm_size: 0x%llx\n", ndev->vm_size);
	seq_printf(m, "scanout_reserved: 0x%llx\n",
		   (u64)NEB_SCANOUT_RESERVED);
	seq_printf(m, "irq: %d\n", ndev->irq);
	seq_printf(m, "open_contexts: %lld\n",
		   atomic64_read(&ndev->open_contexts));
	seq_printf(m, "last_error: 0x%08x\n", READ_ONCE(ndev->last_error));
	seq_printf(m, "reset_count: %lld\n",
		   atomic64_read(&ndev->reset_count));
	seq_printf(m, "last_reset_reason: %u\n", ndev->last_reset_reason);
	seq_printf(m, "last_reset_timestamp_ns: %llu\n",
		   ndev->last_reset_timestamp_ns);
	seq_printf(m, "resetting: %u\n", READ_ONCE(ndev->resetting));
	seq_printf(m, "suspended: %u\n", READ_ONCE(ndev->suspended));
	seq_printf(m, "wedged: %u\n", READ_ONCE(ndev->wedged));

	return 0;
}

static int nebulae_debugfs_faults(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);
	struct drm_nebulae_fault fault;
	struct nebulae_file *nfile;
	unsigned long flags;

	spin_lock_irqsave(&ndev->fault_lock, flags);
	fault = ndev->last_fault;
	spin_unlock_irqrestore(&ndev->fault_lock, flags);
	seq_printf(m,
		   "last sequence=%llu time_ns=%llu ctx=%llu job=%llu asid=%u reason=%u access=0x%x flags=0x%x va=0x%llx token=0x%llx hw_status=%u error=%d\n",
		   fault.sequence, fault.timestamp_ns, fault.ctx_id,
		   fault.job_seq, fault.asid, fault.reason, fault.access,
		   fault.flags, fault.va, fault.replay_token, fault.hw_status,
		   fault.driver_error);

	mutex_lock(&ndev->files_lock);
	list_for_each_entry(nfile, &ndev->files, device_link) {
		u64 next_sequence;
		u32 count;

		spin_lock_irqsave(&nfile->fault_lock, flags);
		count = nfile->fault_count;
		next_sequence = nfile->fault_sequence + 1;
		spin_unlock_irqrestore(&nfile->fault_lock, flags);
		seq_printf(m, "ctx=%llu asid=%u queued=%u next_sequence=%llu\n",
			   nfile->ctx_id, nfile->asid, count, next_sequence);
	}
	mutex_unlock(&ndev->files_lock);
	return 0;
}

static int nebulae_debugfs_registers(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);
	int idx;
	int i;

	if (!drm_dev_enter(&ndev->drm, &idx))
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(nebulae_debugfs_regs); i++) {
		const struct nebulae_debugfs_reg *reg = &nebulae_debugfs_regs[i];

		seq_printf(m, "%-32s 0x%04x: 0x%08x\n", reg->name,
			   reg->reg, readl(ndev->regs + reg->reg));
	}
	drm_dev_exit(idx);

	return 0;
}

static int nebulae_debugfs_vm(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);
	struct drm_printer p = drm_seq_file_printer(m);

	mutex_lock(&ndev->bo_lock);
	drm_mm_print(&ndev->vram_mm, &p);
	mutex_unlock(&ndev->bo_lock);

	return 0;
}

static int nebulae_debugfs_bos(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);
	struct nebulae_bo *bo;
	unsigned int count = 0;
	u64 total = 0;

	mutex_lock(&ndev->bo_lock);
	list_for_each_entry(bo, &ndev->bo_list, link) {
		struct drm_gem_object *obj = &bo->base.base;

		seq_printf(m, "bo%u vram=0x%llx size=%zu flags=0x%08x domain=0x%08x listed=%d\n",
			   count, bo->vram_offset, obj->size, bo->flags,
			   bo->domain, bo->listed);
		total += obj->size;
		count++;
	}
	mutex_unlock(&ndev->bo_lock);

	seq_printf(m, "total_bos: %u\n", count);
	seq_printf(m, "total_bytes: %llu\n", total);

	return 0;
}

static int nebulae_debugfs_submit(struct seq_file *m, void *data)
{
	struct nebulae_device *ndev = nebulae_debugfs_device(m);

	seq_printf(m, "submitted_hw_seq: %lld\n",
		   atomic64_read(&ndev->submitted_jobs));
	seq_printf(m, "completed_hw_seq: %lld\n",
		   atomic64_read(&ndev->completed_jobs));
	seq_printf(m, "scheduled_jobs: %lld\n",
		   atomic64_read(&ndev->scheduled_jobs));
	seq_printf(m, "running_jobs: %lld\n",
		   atomic64_read(&ndev->running_jobs));
	seq_printf(m, "finished_jobs: %lld\n",
		   atomic64_read(&ndev->finished_jobs));
	seq_printf(m, "failed_jobs: %lld\n",
		   atomic64_read(&ndev->failed_jobs));
	seq_printf(m, "signaled_fences: %lld\n",
		   atomic64_read(&ndev->signaled_fences));
	seq_printf(m, "irq_count: %lld\n", atomic64_read(&ndev->irq_count));
	seq_printf(m, "complete_irq_count: %lld\n",
		   atomic64_read(&ndev->complete_irq_count));
	seq_printf(m, "fault_irq_count: %lld\n",
		   atomic64_read(&ndev->fault_irq_count));
	seq_printf(m, "display_irq_count: %lld\n",
		   atomic64_read(&ndev->display_irq_count));
	seq_printf(m, "display_flips: %lld\n",
		   atomic64_read(&ndev->display_flips));
	seq_printf(m, "last_submit_cookie: %llu\n",
		   ndev->last_submit_cookie);
	seq_printf(m, "last_submit_pt_base: 0x%llx\n",
		   ndev->last_submit_pt_base);
	seq_printf(m, "last_submit_asid: %u\n", ndev->last_submit_asid);
	seq_printf(m, "last_irq_status: 0x%08x\n", ndev->last_irq_status);
	seq_printf(m, "last_display_irq_status: 0x%08x\n",
		   ndev->last_display_irq_status);
	seq_printf(m, "last_error: 0x%08x\n", READ_ONCE(ndev->last_error));

	return 0;
}

static const struct drm_info_list nebulae_debugfs_files[] = {
	{ "nebulae_info", nebulae_debugfs_info, 0 },
	{ "nebulae_registers", nebulae_debugfs_registers, 0 },
	{ "nebulae_vm", nebulae_debugfs_vm, 0 },
	{ "nebulae_bos", nebulae_debugfs_bos, 0 },
	{ "nebulae_submit", nebulae_debugfs_submit, 0 },
	{ "nebulae_faults", nebulae_debugfs_faults, 0 },
};

void nebulae_debugfs_init(struct drm_minor *minor)
{
	drm_debugfs_create_files(nebulae_debugfs_files,
				 ARRAY_SIZE(nebulae_debugfs_files),
				 minor->debugfs_root, minor);
}
