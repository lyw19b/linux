// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae render context management.
 */

#include <linux/slab.h>

#include "nebulae_internal.h"

int nebulae_ctx_init(struct nebulae_device *ndev)
{
	INIT_LIST_HEAD(&ndev->files);
	atomic64_set(&ndev->next_ctx_id, 0);
	atomic64_set(&ndev->open_contexts, 0);
	return 0;
}

void nebulae_ctx_fini(struct nebulae_device *ndev)
{
}

static void nebulae_file_release(struct kref *ref)
{
	struct nebulae_file *nfile = container_of(ref, struct nebulae_file,
						 refcount);
	struct nebulae_device *ndev = nfile->ndev;

	/* Closing contexts stay in the reset/restore set while an active job can
	 * still reference their page tables.  The final job reference removes the
	 * context and only then recycles its VM and ASID. */
	mutex_lock(&ndev->files_lock);
	if (!list_empty(&nfile->device_link))
		list_del_init(&nfile->device_link);
	mutex_unlock(&ndev->files_lock);

	WARN_ON(!xa_empty(&nfile->jobs));
	xa_destroy(&nfile->jobs);
	nebulae_vm_file_fini(ndev, nfile);
	if (nfile->asid)
		nebulae_mmu_ctx_free(ndev, nfile->asid);
	atomic64_dec(&ndev->open_contexts);
	kfree(nfile);
}

bool nebulae_file_get(struct nebulae_file *nfile)
{
	return nfile && kref_get_unless_zero(&nfile->refcount);
}

void nebulae_file_put(struct nebulae_file *nfile)
{
	if (nfile)
		kref_put(&nfile->refcount, nebulae_file_release);
}

static int nebulae_file_open_active(struct drm_device *drm,
				    struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_gpu_scheduler *sched = &ndev->scheduler;
	struct nebulae_file *nfile;
	int ret;

	nfile = kzalloc(sizeof(*nfile), GFP_KERNEL);
	if (!nfile)
		return -ENOMEM;
	INIT_LIST_HEAD(&nfile->device_link);
	kref_init(&nfile->refcount);
	nfile->ndev = ndev;
	mutex_init(&nfile->submit_lock);
	spin_lock_init(&nfile->fault_lock);
	xa_init(&nfile->jobs);
	ret = nebulae_vm_file_init(nfile);
	if (ret) {
		xa_destroy(&nfile->jobs);
		kfree(nfile);
		return ret;
	}

	ret = drm_sched_entity_init(&nfile->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL, &sched, 1,
				    NULL);
	if (ret) {
		nebulae_vm_file_fini(ndev, nfile);
		xa_destroy(&nfile->jobs);
		kfree(nfile);
		return ret;
	}

	nfile->ctx_id = atomic64_inc_return(&ndev->next_ctx_id);
	/* Isolation is a creation invariant.  ASID 0 is reserved for the kernel
	 * compatibility domain and must never be a silent fallback for userspace. */
	ret = nebulae_mmu_ctx_alloc(ndev, nfile);
	if (ret) {
		drm_sched_entity_destroy(&nfile->sched_entity);
		nebulae_vm_file_fini(ndev, nfile);
		xa_destroy(&nfile->jobs);
		kfree(nfile);
		return ret;
	}
	atomic64_set(&nfile->submits, 0);
	atomic64_set(&nfile->next_job_seq, 0);
	file->driver_priv = nfile;
	mutex_lock(&ndev->files_lock);
	list_add_tail(&nfile->device_link, &ndev->files);
	mutex_unlock(&ndev->files_lock);
	atomic64_inc(&ndev->open_contexts);

	return 0;
}

int nebulae_file_open(struct drm_device *drm, struct drm_file *file)
{
	int idx;
	int ret;

	ret = nebulae_device_enter(to_nebulae(drm), &idx);
	if (ret)
		return ret;
	ret = nebulae_file_open_active(drm, file);
	nebulae_device_exit(to_nebulae(drm), idx);
	return ret;
}

void nebulae_file_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;

	if (!nfile)
		return;

	mutex_lock(&nfile->submit_lock);
	WRITE_ONCE(nfile->closing, true);
	mutex_unlock(&nfile->submit_lock);
	file->driver_priv = NULL;

	/* Make an executing job observe KILL promptly.  The QEMU/simx contract
	 * acknowledges it only after the whole transient engine is quiescent. */
	nebulae_submit_file_kill_active(ndev, nfile);
	drm_sched_entity_destroy(&nfile->sched_entity);
	/* Jobs own references to the context/GPUVM.  Process exit never waits for
	 * a device fence; the final job performs deferred VMA and ASID teardown. */
	nebulae_file_put(nfile);
}
