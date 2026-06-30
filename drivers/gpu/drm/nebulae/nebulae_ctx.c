// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae render context management.
 */

#include <linux/slab.h>

#include "nebulae_internal.h"

int nebulae_ctx_init(struct nebulae_device *ndev)
{
	atomic64_set(&ndev->next_ctx_id, 0);
	atomic64_set(&ndev->open_contexts, 0);
	return 0;
}

void nebulae_ctx_fini(struct nebulae_device *ndev)
{
}

int nebulae_file_open(struct drm_device *drm, struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct drm_gpu_scheduler *sched = &ndev->scheduler;
	struct nebulae_file *nfile;
	int ret;

	nfile = kzalloc(sizeof(*nfile), GFP_KERNEL);
	if (!nfile)
		return -ENOMEM;

	ret = drm_sched_entity_init(&nfile->sched_entity,
				    DRM_SCHED_PRIORITY_NORMAL, &sched, 1,
				    NULL);
	if (ret) {
		kfree(nfile);
		return ret;
	}

	nfile->ctx_id = atomic64_inc_return(&ndev->next_ctx_id);
	atomic64_set(&nfile->submits, 0);
	file->driver_priv = nfile;
	atomic64_inc(&ndev->open_contexts);

	return 0;
}

void nebulae_file_postclose(struct drm_device *drm, struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;

	if (!nfile)
		return;

	file->driver_priv = NULL;
	drm_sched_entity_destroy(&nfile->sched_entity);
	atomic64_dec(&ndev->open_contexts);
	kfree(nfile);
}
