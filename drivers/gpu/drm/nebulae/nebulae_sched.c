// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae DRM scheduler integration.
 */

#include "nebulae_internal.h"

int nebulae_sched_init(struct nebulae_device *ndev)
{
	atomic64_set(&ndev->scheduled_jobs, 0);
	atomic64_set(&ndev->running_jobs, 0);
	atomic64_set(&ndev->finished_jobs, 0);
	atomic64_set(&ndev->failed_jobs, 0);
	return 0;
}

void nebulae_sched_fini(struct nebulae_device *ndev)
{
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
