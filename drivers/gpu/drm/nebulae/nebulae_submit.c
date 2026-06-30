// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae command submission and IRQ handling.
 */

#include <linux/errno.h>
#include <linux/io.h>

#include <asm/unaligned.h>

#include <drm/drm_gem.h>

#include "nebulae_internal.h"

#define NEB_INTERNAL_CMD_OFFSET		(NEB_SCANOUT_RESERVED - PAGE_SIZE)
#define NEB_USER_CMD_SIZE		128
#define NEB_USER_CMD_CP_EXEC		3
#define NEB_USER_CMD_CP_RING_BASE	16
#define NEB_USER_CMD_CP_RING_SIZE	24
#define NEB_USER_CMD_CP_PACKET_OFFSET	32
#define NEB_USER_CMD_CP_PACKET_SIZE	40
#define NEB_USER_CMD_CP_PACKET_COUNT	44
#define NEB_USER_CMD_CP_PT_BASE		64
#define NEB_USER_CMD_CP_ASID		72

static void neb_cmd_put_u32(u8 *cmd, size_t off, u32 value)
{
	put_unaligned_le32(value, cmd + off);
}

static void neb_cmd_put_u64(u8 *cmd, size_t off, u64 value)
{
	put_unaligned_le64(value, cmd + off);
}

static int nebulae_hw_submit_cmd_bo(struct nebulae_device *ndev,
				    struct nebulae_bo *cmd_bo,
				    u32 offset, u32 size, u32 cmd_count,
				    u64 *seq, u32 *hw_status)
{
	struct drm_gem_object *obj = &cmd_bo->base.base;
	u8 cmd[NEB_USER_CMD_SIZE];
	u64 ring_size;
	u64 completed;

	if (!size || !cmd_count)
		return -EINVAL;
	if ((u64)offset + size > obj->size)
		return -EINVAL;
	if (!cmd_bo->va)
		return -EINVAL;

	ring_size = obj->size;

	memset(cmd, 0, sizeof(cmd));
	neb_cmd_put_u32(cmd, 0, NEB_USER_CMD_CP_EXEC);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_BASE, cmd_bo->va);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_SIZE, ring_size);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_OFFSET, offset);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_SIZE, size);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_COUNT, cmd_count);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_PT_BASE, 0);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_ASID, 0);

	mutex_lock(&ndev->submit_lock);
	memcpy_toio(ndev->vram + NEB_INTERNAL_CMD_OFFSET, cmd, sizeof(cmd));
	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	neb_writeq(ndev, NEB_REG_SUBMIT_OFFSET_LO, NEB_INTERNAL_CMD_OFFSET);
	writel(sizeof(cmd), ndev->regs + NEB_REG_SUBMIT_SIZE);
	writel(1, ndev->regs + NEB_REG_DOORBELL);

	*seq = neb_readq(ndev, NEB_REG_JOB_SEQ_LO);
	completed = neb_readq(ndev, NEB_REG_COMPLETED_SEQ_LO);
	*hw_status = readl(ndev->regs + NEB_REG_LAST_ERROR);
	WRITE_ONCE(ndev->last_error, *hw_status);
	atomic64_set(&ndev->submitted_jobs, *seq);
	atomic64_set(&ndev->completed_jobs, completed);
	mutex_unlock(&ndev->submit_lock);

	if (completed != *seq)
		return -ETIMEDOUT;

	return 0;
}

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
	struct drm_nebulae_submit_cmd_bo *args = data;
	struct drm_gem_object *obj;
	struct nebulae_bo *bo;
	u32 unsupported_flags = DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ |
				DRM_NEBULAE_SUBMIT_IN_FENCE_FD |
				DRM_NEBULAE_SUBMIT_OUT_FENCE_FD;
	u32 hw_status = 0;
	u64 seq;
	int ret;

	if (args->flags & unsupported_flags)
		return -EOPNOTSUPP;
	if (!args->handle || !args->size || !args->cmd_count)
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	if ((u64)args->offset + args->size > obj->size) {
		ret = -EINVAL;
		goto out_put;
	}

	bo = to_nebulae_bo(obj);

	ret = nebulae_sync_all_bos_to_vram(ndev);
	if (ret)
		goto out_put;

	ret = nebulae_hw_submit_cmd_bo(ndev, bo, args->offset, args->size,
				       args->cmd_count, &seq, &hw_status);
	if (!ret)
		ret = nebulae_sync_all_bos_from_vram(ndev);
	args->seq = seq;
	args->status = hw_status;
	args->driver_error = ret;

out_put:
	drm_gem_object_put(obj);
	return ret;
}
