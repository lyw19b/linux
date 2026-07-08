// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae command submission and IRQ handling.
 */

#include <linux/dma-fence.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/sync_file.h>

#include <asm/unaligned.h>

#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>

#include "nebulae_internal.h"

#define NEB_INTERNAL_CQ_OFFSET		(NEB_SCANOUT_RESERVED - PAGE_SIZE)
#define NEB_INTERNAL_QUEUE_DESC_OFFSET	(NEB_SCANOUT_RESERVED - 2 * PAGE_SIZE)
#define NEB_INTERNAL_CMD_OFFSET		(NEB_SCANOUT_RESERVED - 3 * PAGE_SIZE)
#define NEB_USER_CMD_SIZE		128
#define NEB_USER_CMD_CP_EXEC		3
#define NEB_USER_CMD_CP_RING_BASE	16
#define NEB_USER_CMD_CP_RING_SIZE	24
#define NEB_USER_CMD_CP_PACKET_OFFSET	32
#define NEB_USER_CMD_CP_PACKET_SIZE	40
#define NEB_USER_CMD_CP_PACKET_COUNT	44
#define NEB_USER_CMD_CP_COOKIE		48
#define NEB_USER_CMD_CP_PT_BASE		64
#define NEB_USER_CMD_CP_ASID		72

#define NEB_QUEUE_DESC_SIZE		128
#define NEB_QUEUE_DESC_MAGIC		0x5142454e
#define NEB_QUEUE_DESC_VERSION		1
#define NEB_QUEUE_DESC_VERSION_OFF	4
#define NEB_QUEUE_DESC_QUEUE_ID		12
#define NEB_QUEUE_DESC_RING_BASE	16
#define NEB_QUEUE_DESC_RING_SIZE	24
#define NEB_QUEUE_DESC_PACKET_OFFSET	32
#define NEB_QUEUE_DESC_PACKET_SIZE	36
#define NEB_QUEUE_DESC_PACKET_COUNT	40
#define NEB_QUEUE_DESC_CQ_BASE		48
#define NEB_QUEUE_DESC_CQ_SIZE		56
#define NEB_QUEUE_DESC_CQ_ENTRY_SIZE	60
#define NEB_QUEUE_DESC_COOKIE		64
#define NEB_QUEUE_DESC_PT_BASE		72
#define NEB_QUEUE_DESC_ASID		80

#define NEB_CQ_ENTRY_SIZE		32
#define NEB_CQ_ENTRY_MAGIC		0x4342454e
#define NEB_CQ_STATUS			4
#define NEB_CQ_COOKIE			8
#define NEB_CQ_HW_SEQ			16
#define NEB_CQ_COMPLETED_SEQ		24

struct nebulae_fence {
	struct dma_fence base;
};

static const char *nebulae_fence_get_driver_name(struct dma_fence *fence)
{
	return DRIVER_NAME;
}

static const char *nebulae_fence_get_timeline_name(struct dma_fence *fence)
{
	return "submit";
}

static void nebulae_fence_value_str(struct dma_fence *fence, char *str,
				    int size)
{
	snprintf(str, size, "%llu", fence->seqno);
}

static void nebulae_fence_timeline_value_str(struct dma_fence *fence,
					     char *str, int size)
{
	snprintf(str, size, "%llu",
		 dma_fence_is_signaled(fence) ? fence->seqno : 0);
}

static const struct dma_fence_ops nebulae_fence_ops = {
	.use_64bit_seqno = true,
	.get_driver_name = nebulae_fence_get_driver_name,
	.get_timeline_name = nebulae_fence_get_timeline_name,
	.fence_value_str = nebulae_fence_value_str,
	.timeline_value_str = nebulae_fence_timeline_value_str,
};

static struct dma_fence *nebulae_fence_create(struct nebulae_device *ndev)
{
	struct nebulae_fence *nfence;

	nfence = kzalloc(sizeof(*nfence), GFP_KERNEL);
	if (!nfence)
		return NULL;

	dma_fence_init(&nfence->base, &nebulae_fence_ops, &ndev->fence_lock,
		       ndev->fence_context,
		       atomic64_inc_return(&ndev->fence_seqno));

	return &nfence->base;
}

static int nebulae_wait_fence(struct dma_fence *fence, u64 timeout_ns)
{
	signed long ret;
	u64 timeout;

	if (!timeout_ns)
		return dma_fence_wait(fence, true);

	timeout = nsecs_to_jiffies64(timeout_ns);
	if (!timeout)
		timeout = 1;

	ret = dma_fence_wait_timeout(fence, true, timeout);
	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return ret;

	return 0;
}

static int nebulae_wait_hw_complete(struct nebulae_device *ndev, u64 seq)
{
	u64 completed;
	int ret;

	completed = neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO);
	if (completed >= seq) {
		atomic64_set(&ndev->completed_jobs, completed);
		return 0;
	}

	ret = wait_event_interruptible(ndev->submit_wait,
				       atomic64_read(&ndev->completed_jobs) >= seq);
	if (ret)
		return ret;

	return 0;
}

static int nebulae_export_out_fence(struct drm_file *file,
				    struct drm_nebulae_submit_cmd_bo *args,
				    struct dma_fence *fence)
{
	struct drm_syncobj *syncobj = NULL;
	struct sync_file *sync_file;
	int fd = -1;
	int ret = 0;

	if (args->flags & DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ) {
		if (!args->out_syncobj)
			return -EINVAL;

		syncobj = drm_syncobj_find(file, args->out_syncobj);
		if (!syncobj)
			return -ENOENT;
	}

	if (args->flags & DRM_NEBULAE_SUBMIT_OUT_FENCE_FD) {
		fd = get_unused_fd_flags(O_CLOEXEC);
		if (fd < 0) {
			ret = fd;
			goto out_put_syncobj;
		}

		sync_file = sync_file_create(fence);
		if (!sync_file) {
			put_unused_fd(fd);
			ret = -ENOMEM;
			goto out_put_syncobj;
		}
	}

	if (syncobj)
		drm_syncobj_replace_fence(syncobj, fence);

	if (args->flags & DRM_NEBULAE_SUBMIT_OUT_FENCE_FD) {
		fd_install(fd, sync_file->file);
		args->out_fence_fd = fd;
	}

out_put_syncobj:
	if (syncobj)
		drm_syncobj_put(syncobj);

	return ret;
}

static int nebulae_job_add_deps(struct drm_file *file,
				struct drm_nebulae_submit_cmd_bo *args,
				struct nebulae_job *job)
{
	struct dma_fence *fence;
	int ret;

	if (args->in_syncobj) {
		ret = drm_sched_job_add_syncobj_dependency(&job->base, file,
							   args->in_syncobj,
							   0);
		if (ret)
			return ret;
	}

	if (args->flags & DRM_NEBULAE_SUBMIT_IN_FENCE_FD) {
		if (args->in_fence_fd < 0)
			return -EINVAL;

		fence = sync_file_get_fence(args->in_fence_fd);
		if (!fence)
			return -EINVAL;

		return drm_sched_job_add_dependency(&job->base, fence);
	}

	return 0;
}

static void neb_cmd_put_u32(u8 *cmd, size_t off, u32 value)
{
	put_unaligned_le32(value, cmd + off);
}

static void neb_cmd_put_u64(u8 *cmd, size_t off, u64 value)
{
	put_unaligned_le64(value, cmd + off);
}

static u32 neb_cmd_get_u32(const u8 *cmd, size_t off)
{
	return get_unaligned_le32(cmd + off);
}

static u64 neb_cmd_get_u64(const u8 *cmd, size_t off)
{
	return get_unaligned_le64(cmd + off);
}

static int nebulae_hw_submit_cmd_bo(struct nebulae_device *ndev,
				    struct nebulae_bo *cmd_bo,
				    u32 offset, u32 size, u32 cmd_count,
				    u64 pt_base, u32 asid, u64 cookie,
				    u64 *seq, u32 *hw_status)
{
	struct drm_gem_object *obj = &cmd_bo->base.base;
	u8 cmd[NEB_USER_CMD_SIZE];
	u8 desc[NEB_QUEUE_DESC_SIZE];
	u8 cq[NEB_CQ_ENTRY_SIZE];
	u64 ring_size;
	u64 completed;
	u32 cq_status = 0;
	int ret;

	if (!size || !cmd_count)
		return -EINVAL;
	if ((u64)offset + size > obj->size)
		return -EINVAL;
	if (!cmd_bo->va)
		return -EINVAL;

	ring_size = obj->size;

	memset(desc, 0, sizeof(desc));
	neb_cmd_put_u32(desc, 0, NEB_QUEUE_DESC_MAGIC);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_VERSION_OFF,
			NEB_QUEUE_DESC_VERSION);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_QUEUE_ID, 1);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_RING_BASE, cmd_bo->va);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_RING_SIZE, ring_size);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_OFFSET, offset);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_SIZE, size);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_COUNT, cmd_count);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_CQ_BASE, NEB_INTERNAL_CQ_OFFSET);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_CQ_SIZE, NEB_CQ_ENTRY_SIZE);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_CQ_ENTRY_SIZE, NEB_CQ_ENTRY_SIZE);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_COOKIE, cookie);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_PT_BASE, pt_base);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_ASID, asid);

	memset(cmd, 0, sizeof(cmd));
	neb_cmd_put_u32(cmd, 0, NEB_USER_CMD_CP_EXEC);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_BASE,
			NEB_INTERNAL_QUEUE_DESC_OFFSET);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_SIZE, sizeof(desc));
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_OFFSET, 0);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_SIZE, sizeof(desc));
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_COUNT, 1);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_COOKIE, cookie);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_PT_BASE, pt_base);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_ASID, asid);

	mutex_lock(&ndev->submit_lock);
	ndev->last_submit_cookie = cookie;
	ndev->last_submit_pt_base = pt_base;
	ndev->last_submit_asid = asid;
	memset_io(ndev->vram + NEB_INTERNAL_CQ_OFFSET, 0, NEB_CQ_ENTRY_SIZE);
	memcpy_toio(ndev->vram + NEB_INTERNAL_QUEUE_DESC_OFFSET, desc,
		    sizeof(desc));
	memcpy_toio(ndev->vram + NEB_INTERNAL_CMD_OFFSET, cmd, sizeof(cmd));
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	neb_writeq(ndev, NEB_REG_SUBMIT_OFFSET_LO, NEB_INTERNAL_CMD_OFFSET);
	writel(sizeof(cmd), ndev->regs + NEB_REG_SUBMIT_SIZE);
	writel(1, ndev->regs + NEB_REG_DOORBELL);

	*seq = neb_readq(ndev, NEB_REG_JOB_SEQ_LO);
	atomic64_set(&ndev->submitted_jobs, *seq);

	ret = nebulae_wait_hw_complete(ndev, *seq);
	completed = atomic64_read(&ndev->completed_jobs);
	*hw_status = readl(ndev->regs + NEB_REG_LAST_ERROR);
	WRITE_ONCE(ndev->last_error, *hw_status);
	memcpy_fromio(cq, ndev->vram + NEB_INTERNAL_CQ_OFFSET, sizeof(cq));
	mutex_unlock(&ndev->submit_lock);

	if (ret)
		return ret;
	if (completed < *seq)
		return -ETIMEDOUT;

	if (neb_cmd_get_u32(cq, 0) == NEB_CQ_ENTRY_MAGIC &&
	    neb_cmd_get_u64(cq, NEB_CQ_COOKIE) == cookie) {
		cq_status = neb_cmd_get_u32(cq, NEB_CQ_STATUS);
		*seq = neb_cmd_get_u64(cq, NEB_CQ_HW_SEQ);
		completed = neb_cmd_get_u64(cq, NEB_CQ_COMPLETED_SEQ);
		*hw_status = cq_status;
		atomic64_set(&ndev->completed_jobs, completed);
	}

	if (cq_status)
		return -EIO;

	return 0;
}

static void nebulae_job_release(struct kref *kref)
{
	struct nebulae_job *job = container_of(kref, struct nebulae_job,
					      refcount);

	kfree(job);
}

static void nebulae_job_get(struct nebulae_job *job)
{
	kref_get(&job->refcount);
}

static void nebulae_job_put(struct nebulae_job *job)
{
	kref_put(&job->refcount, nebulae_job_release);
}

static int nebulae_submit_cmd_bo_direct(struct nebulae_device *ndev,
					struct nebulae_file *nfile,
					struct drm_nebulae_submit_cmd_bo *args,
					struct nebulae_bo *bo)
{
	u64 seq = 0;
	u32 status = 0;
	u64 cookie;
	int ret;

	/*
	 * The QEMU simx graphics path completes command submissions
	 * synchronously.  Avoid queuing the simple no-fence path through the DRM
	 * scheduler workqueue, otherwise Xorg's Present copy path can build a
	 * backlog while it is itself waiting for the submitted fence.
	 */
	cookie = atomic64_inc_return(&nfile->submits);
	nebulae_sched_record_submit(ndev);

	ret = nebulae_bo_sync_to_vram(ndev, bo);
	if (!ret)
		ret = nebulae_hw_submit_cmd_bo(ndev, bo, args->offset,
					       args->size, args->cmd_count,
					       nfile->mmu_root, nfile->asid,
					       cookie, &seq, &status);

	nebulae_sched_record_complete(ndev, ret);

	args->seq = seq ?: cookie;
	args->status = status;
	args->driver_error = ret;
	return ret;
}

static struct dma_fence *nebulae_job_run(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);
	struct nebulae_device *ndev = job->ndev;
	struct dma_fence *fence;
	int ret;

	if (unlikely(job->base.s_fence->finished.error))
		return NULL;

	ret = nebulae_bo_sync_to_vram(ndev, job->cmd_bo);
	if (!ret)
		ret = nebulae_hw_submit_cmd_bo(ndev, job->cmd_bo, job->offset,
					       job->size, job->cmd_count,
					       job->pt_base, job->asid,
					       job->cookie,
					       &job->hw_seq,
					       &job->hw_status);

	job->result = ret;

	fence = nebulae_fence_create(ndev);
	if (!fence) {
		job->result = -ENOMEM;
		nebulae_sched_record_complete(ndev, job->result);
		return ERR_PTR(-ENOMEM);
	}

	if (job->result)
		dma_fence_set_error(fence, job->result);

	dma_fence_signal(fence);
	atomic64_inc(&ndev->signaled_fences);
	nebulae_sched_record_complete(ndev, job->result);

	return fence;
}

static enum drm_gpu_sched_stat
nebulae_job_timedout(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);
	struct nebulae_device *ndev = job->ndev;

	WRITE_ONCE(ndev->last_error, ETIMEDOUT);
	job->result = -ETIMEDOUT;
	atomic64_inc(&ndev->failed_jobs);
	drm_sched_fault(&ndev->scheduler);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void nebulae_job_free(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);

	drm_sched_job_cleanup(sched_job);

	if (job->cmd_obj) {
		drm_gem_object_put(job->cmd_obj);
		job->cmd_obj = NULL;
		job->cmd_bo = NULL;
	}

	nebulae_job_put(job);
}

const struct drm_sched_backend_ops nebulae_gpu_sched_ops = {
	.run_job = nebulae_job_run,
	.timedout_job = nebulae_job_timedout,
	.free_job = nebulae_job_free,
};

int nebulae_ioctl_submit(struct drm_device *drm, void *data,
			 struct drm_file *file)
{
	struct drm_nebulae_submit *args = data;

	if (!args->cmds || !args->size)
		return -EINVAL;
	if (args->flags)
		return -EOPNOTSUPP;

	return -EOPNOTSUPP;
}

int nebulae_ioctl_submit_cmd_bo(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;
	struct drm_nebulae_submit_cmd_bo *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	struct nebulae_job *job;
	struct dma_fence *finished = NULL;
	u32 supported_flags = DRM_NEBULAE_SUBMIT_ASYNC |
			      DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ |
			      DRM_NEBULAE_SUBMIT_IN_FENCE_FD |
			      DRM_NEBULAE_SUBMIT_OUT_FENCE_FD;
	u32 status = 0;
	u64 seq = 0;
	int ret;

	if (!nfile)
		return -EINVAL;
	if (args->flags & ~supported_flags)
		return -EINVAL;
	if (!args->handle || !args->size || !args->cmd_count)
		return -EINVAL;
	if ((args->flags & DRM_NEBULAE_SUBMIT_OUT_FENCE_FD) &&
	    args->out_fence_fd != -1)
		return -EINVAL;
	if (!(args->flags & DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ) &&
	    args->out_syncobj)
		return -EINVAL;
	if ((args->flags & DRM_NEBULAE_SUBMIT_ASYNC) &&
	    !(args->flags & (DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ |
			     DRM_NEBULAE_SUBMIT_OUT_FENCE_FD)))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if ((u64)args->offset + args->size > obj->size) {
		ret = -EINVAL;
		args->driver_error = ret;
		goto out_put;
	}

	bo = to_nebulae_bo(obj);

	if (!args->flags && !args->in_syncobj) {
		ret = nebulae_submit_cmd_bo_direct(ndev, nfile, args, bo);
		goto out_put;
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_put;
	}

	kref_init(&job->refcount);
	job->ndev = ndev;
	job->cmd_obj = obj;
	job->cmd_bo = bo;
	job->offset = args->offset;
	job->size = args->size;
	job->cmd_count = args->cmd_count;

	ret = drm_sched_job_init(&job->base, &nfile->sched_entity, 1, nfile);
	if (ret)
		goto out_free_job;

	ret = nebulae_job_add_deps(file, args, job);
	if (ret)
		goto out_cleanup_job;

	drm_sched_job_arm(&job->base);
	finished = dma_fence_get(&job->base.s_fence->finished);
	seq = job->base.id;
	job->cookie = job->base.id;
	job->asid = nfile->asid;
	job->pt_base = nfile->mmu_root;

	ret = nebulae_export_out_fence(file, args, finished);
	if (ret)
		goto out_cleanup_job;

	nebulae_job_get(job);
	atomic64_inc(&nfile->submits);
	nebulae_sched_record_submit(ndev);
	drm_sched_entity_push_job(&job->base);

	if (!(args->flags & DRM_NEBULAE_SUBMIT_ASYNC)) {
		ret = nebulae_wait_fence(finished, args->timeout_ns);
		if (!ret)
			ret = job->result;

		seq = job->hw_seq ?: seq;
		status = job->hw_status;
	}

	args->seq = seq;
	args->status = status;
	args->driver_error = ret;
	dma_fence_put(finished);
	nebulae_job_put(job);
	return ret;

out_cleanup_job:
	dma_fence_put(finished);
	drm_sched_job_cleanup(&job->base);
out_free_job:
	kfree(job);
out_put:
	drm_gem_object_put(obj);
	return ret;
}
