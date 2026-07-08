// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae DRM scheduler integration.
 */

#include <linux/dma-fence.h>
#include <linux/jiffies.h>

#include "nebulae_internal.h"

#define NEBULAE_JOB_TIMEOUT_MS	60000

int nebulae_sched_init(struct nebulae_device *ndev)
{
	int ret;

	spin_lock_init(&ndev->fence_lock);
	ndev->fence_context = dma_fence_context_alloc(1);
	atomic64_set(&ndev->fence_seqno, 0);
	atomic64_set(&ndev->signaled_fences, 0);
	atomic64_set(&ndev->scheduled_jobs, 0);
	atomic64_set(&ndev->running_jobs, 0);
	atomic64_set(&ndev->finished_jobs, 0);
	atomic64_set(&ndev->failed_jobs, 0);

	ret = drm_sched_init(&ndev->scheduler, &nebulae_gpu_sched_ops, NULL,
			     DRM_SCHED_PRIORITY_COUNT, 1, 0,
			     msecs_to_jiffies(NEBULAE_JOB_TIMEOUT_MS),
			     NULL, NULL, "nebulae-render", ndev->drm.dev);
	if (ret)
		return ret;

	return 0;
}

void nebulae_gpu_sched_fini(struct nebulae_device *ndev)
{
	drm_sched_fini(&ndev->scheduler);
}

void nebulae_sched_record_submit(struct nebulae_device *ndev)
{
	atomic64_inc(&ndev->scheduled_jobs);
	atomic64_inc(&ndev->running_jobs);
}

void nebulae_sched_record_complete(struct nebulae_device *ndev, int ret)
{
	if (atomic64_read(&ndev->running_jobs) > 0)
		atomic64_dec(&ndev->running_jobs);

	if (ret)
		atomic64_inc(&ndev->failed_jobs);
	else
		atomic64_inc(&ndev->finished_jobs);
}
