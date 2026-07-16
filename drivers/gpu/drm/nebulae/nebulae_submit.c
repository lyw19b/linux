// SPDX-License-Identifier: GPL-2.0-only
/*
 * Nebulae command submission and IRQ handling.
 */

#include <linux/dma-fence.h>
#include <linux/dma-resv.h>
#include <linux/errno.h>
#include <linux/file.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/overflow.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/sync_file.h>
#include <linux/string.h>
#include <linux/vmalloc.h>

#include <asm/unaligned.h>

#include <drm/drm_gem.h>
#include <drm/drm_syncobj.h>

#include "nebulae_internal.h"
#include "nebulae_trace.h"

#define NEB_USER_CMD_SIZE		128
#define NEB_USER_CMD_CP_EXEC		3
#define NEB_USER_CMD_CP_FLAGS		4
#define NEB_USER_CMD_CP_FLAG_MAY_WRITE_SCANOUT	BIT(0)
#define NEB_USER_CMD_CP_FLAG_WRITESET_VALID	BIT(31)
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
#define NEB_SUBMIT_META_ALIGN		8
#define NEB_SUBMIT_MAX_BOS		4096

#define NEB_CDP_PACKET_SLOT_BYTES	256
#define NEB_CDP_PKT_NOP		0
#define NEB_CDP_PKT_DRAW_ARRAYS		17
#define NEB_CDP_PKT_DRAW_INDEXED	16
#define NEB_CDP_PKT_FILL		18

#define NEB_CDP_HEADER_RESERVED_MASK	GENMASK_ULL(47, 0)
#define NEB_CDP_DRAW_ARRAYS_WORDS	14
#define NEB_CDP_DRAW_INDEXED_WORDS	16
#define NEB_CDP_FILL_WORDS		3
#define NEB_CDP_DRAW_ARRAYS_VERTEX	24
#define NEB_CDP_DRAW_ARRAYS_FB		32
#define NEB_CDP_DRAW_ARRAYS_DEPTH	40
#define NEB_CDP_DRAW_INDEXED_VERTEX	24
#define NEB_CDP_DRAW_INDEXED_INDEX	32
#define NEB_CDP_DRAW_INDEXED_FB		40
#define NEB_CDP_DRAW_INDEXED_DEPTH	48
#define NEB_CDP_FILL_DST		8
#define NEB_CDP_FILL_SIZE		16
#define NEB_CDP_FILL_PATTERN_SIZE	28

#define NEB_CDP_STAGE_COUNT		7
#define NEB_CDP_MAX_VERTEX_BUFFERS	8
#define NEB_CDP_DESCRIPTOR_CAPACITY	256
#define NEB_CDP_MAX_RESOURCES		NEB_CDP_DESCRIPTOR_CAPACITY
#define NEB_CDP_MAX_SAMPLERS		NEB_CDP_DESCRIPTOR_CAPACITY
#define NEB_CDP_MAX_UBOS		NEB_CDP_DESCRIPTOR_CAPACITY
#define NEB_CDP_MAX_SSBOS		NEB_CDP_DESCRIPTOR_CAPACITY

#define NEB_CDP_DRAW_PIPELINE_STATE	8
#define NEB_CDP_DRAW_DESCRIPTOR		16

#define NEB_CDP_PIPE_SHADER_METADATA	16
#define NEB_CDP_PIPE_VERTEX_STATE	72
#define NEB_CDP_PIPE_RASTER_STATE	80
#define NEB_CDP_PIPE_INTERP_STATE	88
#define NEB_CDP_PIPE_EXPORT_STATE	96
#define NEB_CDP_PIPE_DSB_STATE		104
#define NEB_CDP_PIPE_STREAM_OUTPUT	112

#define NEB_CDP_DDESC_SHADER_PC		0
#define NEB_CDP_DDESC_RESOURCE_TABLE	56
#define NEB_CDP_DDESC_PUSH_CONSTANTS	64
#define NEB_CDP_DDESC_VERTEX_STATE	120
#define NEB_CDP_DDESC_RASTER_STATE	128
#define NEB_CDP_DDESC_INTERP_STATE	136
#define NEB_CDP_DDESC_EXPORT_STATE	144
#define NEB_CDP_DDESC_DSB_STATE		152
#define NEB_CDP_DDESC_STREAM_OUTPUT	160
#define NEB_CDP_DDESC_GS_CONTROL	168
#define NEB_CDP_DDESC_TESS_CONTROL	176

#define NEB_CDP_RESOURCE_TABLE_SIZE	10256
#define NEB_CDP_RT_RESOURCES		16
#define NEB_CDP_RT_SAMPLERS		2064
#define NEB_CDP_RT_UBOS		4112
#define NEB_CDP_RT_SSBOS		6160
#define NEB_CDP_RT_UBO_SIZES		8208
#define NEB_CDP_RT_SSBO_SIZES		9232

#define NEB_CDP_TEXTURE_DESC_SIZE	96
#define NEB_CDP_TEX_BASE_ADDRESS	32
#define NEB_CDP_TEX_MIP_OFFSETS		72
#define NEB_CDP_TEX_SPARSE_RESIDENCY	80
#define NEB_CDP_TEX_WIDTH		8
#define NEB_CDP_TEX_HEIGHT		12
#define NEB_CDP_TEX_STRIDE		16
#define NEB_CDP_TEX_LEVELS		20
#define NEB_CDP_TEX_DEPTH		28
#define NEB_CDP_TEX_LAYERS		48
#define NEB_CDP_TEX_SLICE_STRIDE	52

#define NEB_CDP_VERTEX_STATE_SIZE	464
#define NEB_CDP_VERTEX_STATE_BUF_COUNT	8
#define NEB_CDP_VERTEX_STATE_BUFFERS	272
#define NEB_CDP_VERTEX_BUFFER_SIZE	24
#define NEB_CDP_VERTEX_BUFFER_BASE	0
#define NEB_CDP_VERTEX_BUFFER_BYTES	12

struct nebulae_submit_shadow {
	struct drm_mm_node node;
	struct nebulae_vma *image_vma;
	struct nebulae_vma *cq_vma;
	u64 phys;
	u64 image_va;
	u64 cq_va;
	u64 image_map_size;
	u64 total_size;
	u64 desc_rel;
	u64 cmd_rel;
	u64 cq_rel;
	bool allocated;
};

struct nebulae_submit_validation {
	struct nebulae_job *job;
	u64 scanout_base;
	u64 scanout_size;
	u64 scanout_generation;
	bool scanout_direct;
	bool may_write_scanout;
};

static void nebulae_job_get(struct nebulae_job *job);
static void nebulae_job_put(struct nebulae_job *job);

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

static void nebulae_fence_release(struct dma_fence *fence)
{
	kfree(container_of(fence, struct nebulae_fence, base));
}

static const struct dma_fence_ops nebulae_fence_ops = {
	.use_64bit_seqno = true,
	.get_driver_name = nebulae_fence_get_driver_name,
	.get_timeline_name = nebulae_fence_get_timeline_name,
	.fence_value_str = nebulae_fence_value_str,
	.timeline_value_str = nebulae_fence_timeline_value_str,
	.release = nebulae_fence_release,
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

static bool nebulae_submit_range_has(u64 base, u32 size, u64 va, u64 len)
{
	u64 end;

	if (base > U64_MAX - size)
		return false;
	end = base + size;
	if (va < base || va > end)
		return false;
	if (len > end - va)
		return false;

	return true;
}

static bool nebulae_submit_off_has(u32 off, u32 size, u32 len)
{
	if (off > size)
		return false;
	if (len > size - off)
		return false;

	return true;
}

static bool
nebulae_submit_job_has_bo(const struct nebulae_job *job,
			  const struct nebulae_bo *bo)
{
	u32 i;

	for (i = 0; i < job->obj_count; i++) {
		if (to_nebulae_bo(job->objs[i]) == bo)
			return true;
	}
	return false;
}

static bool nebulae_submit_ranges_overlap(u64 a, u64 asize,
					  u64 b, u64 bsize)
{
	return asize && bsize && a <= U64_MAX - asize &&
	       b <= U64_MAX - bsize && a < b + bsize && b < a + asize;
}

static void
nebulae_submit_snapshot_scanout(struct nebulae_device *ndev,
				struct nebulae_submit_validation *validation)
{
	unsigned long flags;

	spin_lock_irqsave(&ndev->scanout_lock, flags);
	validation->scanout_base = ndev->scanout_base;
	validation->scanout_size = ndev->scanout_size;
	validation->scanout_generation = ndev->scanout_generation;
	validation->scanout_direct = ndev->scanout_direct;
	spin_unlock_irqrestore(&ndev->scanout_lock, flags);
}

static bool
nebulae_submit_external_va_valid(struct nebulae_submit_validation *validation,
				 u64 va, u64 len, u32 prot)
{
	struct nebulae_job *job = validation->job;
	struct nebulae_bo *bo = NULL;

	if (!va)
		return true;
	if (!nebulae_vm_range_valid(job->nfile, va, len, prot, &bo) || !bo)
		return false;
	/* All control data from the command BO must have been copied into the
	 * immutable shadow.  Never follow a mutable pointer back into it. */
	if (bo == job->cmd_bo)
		return false;
	if (!nebulae_submit_job_has_bo(job, bo))
		return false;

	/* The validator already resolves every external writable address through
	 * the submitting VM.  Classify ownership here instead of treating the
	 * entire GPU as a scanout writer or trusting a userspace hint.  BO-granular
	 * matching is deliberately conservative for subranges of the active FB. */
	if ((prot & NEB_VM_PROT_WRITE) && validation->scanout_direct &&
	    nebulae_submit_ranges_overlap(
		    bo->vram_offset, bo->base.base.size,
		    validation->scanout_base, validation->scanout_size))
		validation->may_write_scanout = true;
	return true;
}

static int nebulae_submit_shadow_va_to_off(u64 old_base, u32 image_size,
					   u64 va, u32 len, u32 *off)
{
	if (!nebulae_submit_range_has(old_base, image_size, va, 1))
		return 1;
	if (!nebulae_submit_range_has(old_base, image_size, va, len))
		return -EINVAL;

	*off = (u32)(va - old_base);
	return 0;
}

static int nebulae_submit_validate_address(
		struct nebulae_submit_validation *validation, u64 old_base,
		u32 image_size, u64 va, u64 len, u32 prot)
{
	if (!va)
		return 0;
	if (!len)
		return -EINVAL;
	if (nebulae_submit_range_has(old_base, image_size, va, 1))
		return nebulae_submit_range_has(old_base, image_size, va, len) ?
		       0 : -EINVAL;
	return nebulae_submit_external_va_valid(validation, va, len, prot) ?
		       0 : -EACCES;
}

static int nebulae_submit_shadow_rebase_field(u8 *image, u32 image_size,
					      u64 old_base, u64 new_base,
					      u32 field_off, u64 *old_va,
					      struct nebulae_submit_validation *validation)
{
	u64 va;

	if (!nebulae_submit_off_has(field_off, image_size, sizeof(u64)))
		return -EINVAL;

	va = neb_cmd_get_u64(image, field_off);
	if (old_va)
		*old_va = va;
	if (nebulae_submit_range_has(old_base, image_size, va, 1))
		neb_cmd_put_u64(image, field_off, new_base + (va - old_base));
	else if (!nebulae_submit_external_va_valid(validation, va, 1,
						   NEB_VM_PROT_READ))
		return -EACCES;

	return 0;
}

static int nebulae_submit_shadow_rebase_texture(u8 *image, u32 image_size,
						u64 old_base, u64 new_base,
						u64 texture_va,
						struct nebulae_submit_validation *validation)
{
	u64 base;
	u64 slice_bytes;
	u64 total_bytes;
	u64 slices;
	u32 width;
	u32 height;
	u32 stride;
	u32 levels;
	u32 depth;
	u32 layers;
	u32 slice_stride;
	u32 off;
	int ret;

	ret = nebulae_submit_shadow_va_to_off(old_base, image_size, texture_va,
					      NEB_CDP_TEXTURE_DESC_SIZE, &off);
	if (ret)
		return ret < 0 ? ret : (texture_va ? -EACCES : 0);

	width = neb_cmd_get_u32(image, off + NEB_CDP_TEX_WIDTH);
	height = neb_cmd_get_u32(image, off + NEB_CDP_TEX_HEIGHT);
	stride = neb_cmd_get_u32(image, off + NEB_CDP_TEX_STRIDE);
	levels = neb_cmd_get_u32(image, off + NEB_CDP_TEX_LEVELS);
	depth = neb_cmd_get_u32(image, off + NEB_CDP_TEX_DEPTH);
	layers = neb_cmd_get_u32(image, off + NEB_CDP_TEX_LAYERS);
	slice_stride = neb_cmd_get_u32(image, off + NEB_CDP_TEX_SLICE_STRIDE);
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_TEX_BASE_ADDRESS,
						 &base, validation);
	if (ret)
		return ret;
	if (base) {
		if (!width || !height || !stride)
			return -EINVAL;
		if (check_mul_overflow((u64)stride, (u64)height,
				       &slice_bytes))
			return -EOVERFLOW;
		slices = max_t(u64, max(depth, layers), 1);
		if (slice_stride && slices > 1) {
			if (check_mul_overflow(slices - 1, (u64)slice_stride,
					       &total_bytes) ||
			    check_add_overflow(total_bytes, slice_bytes,
					       &total_bytes))
				return -EOVERFLOW;
		} else if (check_mul_overflow(slice_bytes, slices,
					      &total_bytes)) {
			return -EOVERFLOW;
		}
		ret = nebulae_submit_validate_address(validation, old_base,
				image_size, base, total_bytes, NEB_VM_PROT_READ);
		if (ret)
			return ret;
	}
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_TEX_MIP_OFFSETS,
						 &base, validation);
	if (ret)
		return ret;
	if (base && levels) {
		ret = nebulae_submit_validate_address(validation, old_base,
				image_size, base, (u64)levels * sizeof(u64),
				NEB_VM_PROT_READ);
		if (ret)
			return ret;
	}

	return nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						  new_base,
						  off + NEB_CDP_TEX_SPARSE_RESIDENCY,
						  NULL, validation);
}

static int nebulae_submit_shadow_rebase_resource_table(u8 *image,
						       u32 image_size,
						       u64 old_base,
						       u64 new_base,
						       u64 table_va,
						       struct nebulae_submit_validation *validation)
{
	u32 resource_count;
	u32 sampler_count;
	u32 off;
	u64 va;
	int ret;
	u32 i;

	ret = nebulae_submit_shadow_va_to_off(old_base, image_size, table_va,
					      NEB_CDP_RESOURCE_TABLE_SIZE,
					      &off);
	if (ret)
		return ret < 0 ? ret : (table_va ? -EACCES : 0);

	resource_count = neb_cmd_get_u32(image + off, 4);
	sampler_count = neb_cmd_get_u32(image + off, 8);
	if (resource_count > NEB_CDP_MAX_RESOURCES ||
	    sampler_count > NEB_CDP_MAX_SAMPLERS)
		return -EINVAL;

	for (i = 0; i < resource_count; i++) {
		ret = nebulae_submit_shadow_rebase_field(image, image_size,
							 old_base, new_base,
							 off + NEB_CDP_RT_RESOURCES +
							 i * sizeof(u64), &va,
							 validation);
		if (ret)
			return ret;
		ret = nebulae_submit_shadow_rebase_texture(image, image_size,
							   old_base, new_base,
							   va, validation);
		if (ret)
			return ret;
	}

	for (i = 0; i < sampler_count; i++) {
		ret = nebulae_submit_shadow_rebase_field(image, image_size,
							 old_base, new_base,
							 off + NEB_CDP_RT_SAMPLERS +
							 i * sizeof(u64), &va,
							 validation);
		if (ret)
			return ret;
		if (va && !nebulae_submit_range_has(old_base, image_size, va, 48))
			return -EACCES;
	}

	for (i = 0; i < NEB_CDP_MAX_UBOS; i++) {
		u32 bytes = neb_cmd_get_u32(image,
				off + NEB_CDP_RT_UBO_SIZES + i * sizeof(u32));

		ret = nebulae_submit_shadow_rebase_field(image, image_size,
							 old_base, new_base,
							 off + NEB_CDP_RT_UBOS +
							 i * sizeof(u64), &va,
							 validation);
		if (ret)
			return ret;
		ret = nebulae_submit_validate_address(validation, old_base,
				image_size, va, bytes ?: 1, NEB_VM_PROT_READ);
		if (ret)
			return ret;
	}

	for (i = 0; i < NEB_CDP_MAX_SSBOS; i++) {
		u32 bytes = neb_cmd_get_u32(image,
				off + NEB_CDP_RT_SSBO_SIZES + i * sizeof(u32));

		ret = nebulae_submit_shadow_rebase_field(image, image_size,
							 old_base, new_base,
							 off + NEB_CDP_RT_SSBOS +
							 i * sizeof(u64), &va,
							 validation);
		if (ret)
			return ret;
		ret = nebulae_submit_validate_address(validation, old_base,
				image_size, va, bytes ?: 1, NEB_VM_PROT_WRITE);
		if (ret)
			return ret;
	}

	return 0;
}

static int nebulae_submit_shadow_rebase_vertex_state(u8 *image,
						     u32 image_size,
						     u64 old_base,
						     u64 new_base,
						     u64 state_va,
						     struct nebulae_submit_validation *validation)
{
	u32 buffer_count;
	u32 off;
	int ret;
	u32 i;

	ret = nebulae_submit_shadow_va_to_off(old_base, image_size, state_va,
					      NEB_CDP_VERTEX_STATE_SIZE, &off);
	if (ret)
		return ret < 0 ? ret : (state_va ? -EACCES : 0);

	buffer_count = neb_cmd_get_u32(image + off,
				       NEB_CDP_VERTEX_STATE_BUF_COUNT);
	if (buffer_count > NEB_CDP_MAX_VERTEX_BUFFERS)
		return -EINVAL;

	for (i = 0; i < buffer_count; i++) {
		u32 bytes = neb_cmd_get_u32(image,
				off + NEB_CDP_VERTEX_STATE_BUFFERS +
				i * NEB_CDP_VERTEX_BUFFER_SIZE +
				NEB_CDP_VERTEX_BUFFER_BYTES);
		u64 va;

		ret = nebulae_submit_shadow_rebase_field(image, image_size,
				old_base, new_base,
				off + NEB_CDP_VERTEX_STATE_BUFFERS +
				i * NEB_CDP_VERTEX_BUFFER_SIZE +
					NEB_CDP_VERTEX_BUFFER_BASE, &va,
					validation);
		if (ret)
			return ret;
		ret = nebulae_submit_validate_address(validation, old_base,
				image_size, va, bytes ?: 1, NEB_VM_PROT_READ);
		if (ret)
			return ret;
	}

	return 0;
}

static int nebulae_submit_shadow_rebase_pipeline(u8 *image, u32 image_size,
						 u64 old_base, u64 new_base,
						 u64 pipeline_va,
						 struct nebulae_submit_validation *validation)
{
	u32 off;
	u64 va;
	int ret;
	u32 i;

	ret = nebulae_submit_shadow_va_to_off(old_base, image_size, pipeline_va,
					      NEB_CDP_PIPE_STREAM_OUTPUT +
					      sizeof(u64), &off);
	if (ret)
		return ret < 0 ? ret : (pipeline_va ? -EACCES : 0);

	for (i = 0; i < NEB_CDP_STAGE_COUNT; i++) {
		ret = nebulae_submit_shadow_rebase_field(image, image_size,
				old_base, new_base,
				off + NEB_CDP_PIPE_SHADER_METADATA +
					i * sizeof(u64), NULL, validation);
		if (ret)
			return ret;
	}

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_PIPE_VERTEX_STATE,
						 &va, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_vertex_state(image, image_size,
							old_base, new_base, va,
							validation);
	if (ret)
		return ret;

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_PIPE_RASTER_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_PIPE_INTERP_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_PIPE_EXPORT_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_PIPE_DSB_STATE,
						 NULL, validation);
	if (ret)
		return ret;

	return nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						  new_base,
						  off + NEB_CDP_PIPE_STREAM_OUTPUT,
						  NULL, validation);
}

static int nebulae_submit_shadow_rebase_draw_desc(u8 *image, u32 image_size,
						  u64 old_base, u64 new_base,
						  u64 draw_desc_va,
						  struct nebulae_submit_validation *validation)
{
	u32 off;
	u64 va;
	int ret;
	u32 i;

	ret = nebulae_submit_shadow_va_to_off(old_base, image_size, draw_desc_va,
					      NEB_CDP_DDESC_TESS_CONTROL +
					      sizeof(u64), &off);
	if (ret)
		return ret < 0 ? ret : (draw_desc_va ? -EACCES : 0);

	for (i = 0; i < NEB_CDP_STAGE_COUNT; i++) {
		ret = nebulae_submit_shadow_rebase_field(image, image_size,
				old_base, new_base,
				off + NEB_CDP_DDESC_SHADER_PC +
					i * sizeof(u64), NULL, validation);
		if (ret)
			return ret;
		ret = nebulae_submit_shadow_rebase_field(image, image_size,
				old_base, new_base,
				off + NEB_CDP_DDESC_PUSH_CONSTANTS +
					i * sizeof(u64), NULL, validation);
		if (ret)
			return ret;
	}

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_RESOURCE_TABLE,
						 &va, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_resource_table(image, image_size,
							  old_base, new_base,
							  va, validation);
	if (ret)
		return ret;

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_VERTEX_STATE,
						 &va, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_vertex_state(image, image_size,
							old_base, new_base, va,
							validation);
	if (ret)
		return ret;

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_RASTER_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_INTERP_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_EXPORT_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_DSB_STATE,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_STREAM_OUTPUT,
						 NULL, validation);
	if (ret)
		return ret;
	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base,
						 off + NEB_CDP_DDESC_GS_CONTROL,
						 NULL, validation);
	if (ret)
		return ret;

	return nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						  new_base,
						  off + NEB_CDP_DDESC_TESS_CONTROL,
						  NULL, validation);
}

static int
nebulae_submit_validate_leaf(u8 *image, u32 image_size, u64 old_base,
			     u64 new_base, u32 field_off, u64 len, u32 prot,
			     struct nebulae_submit_validation *validation)
{
	u64 va;
	int ret;

	ret = nebulae_submit_shadow_rebase_field(image, image_size, old_base,
						 new_base, field_off, &va,
						 validation);
	if (ret || !va)
		return ret;
	if (nebulae_submit_range_has(old_base, image_size, va, 1))
		return nebulae_submit_range_has(old_base, image_size, va, len) ?
		       0 : -EINVAL;
	return nebulae_submit_external_va_valid(validation, va, len, prot) ?
	       0 : -EACCES;
}

static int nebulae_submit_shadow_rebase_packets(u8 *image, u32 image_size,
						u32 packet_count, u64 old_base,
						u64 new_base,
						struct nebulae_submit_validation *validation)
{
	u64 pipeline_va;
	u64 draw_desc_va;
	u32 slot_off;
	u64 header;
	u64 fill_size;
	u32 payload_words;
	u32 expected_words;
	u8 type;
	int ret;
	u32 i;

	if ((u64)packet_count * NEB_CDP_PACKET_SLOT_BYTES > image_size)
		return -EINVAL;

	for (i = 0; i < packet_count; i++) {
		slot_off = i * NEB_CDP_PACKET_SLOT_BYTES;
		if (!nebulae_submit_off_has(slot_off, image_size, sizeof(u64)))
			return -EINVAL;

		header = neb_cmd_get_u64(image, slot_off);
		type = header >> 56;
		payload_words = (header >> 48) & 0xff;
		if (header & NEB_CDP_HEADER_RESERVED_MASK)
			return -EINVAL;
		switch (type) {
		case NEB_CDP_PKT_NOP:
			expected_words = 0;
			break;
		case NEB_CDP_PKT_FILL:
			expected_words = NEB_CDP_FILL_WORDS;
			fill_size = neb_cmd_get_u64(image,
						       slot_off + NEB_CDP_FILL_SIZE);
			if (!fill_size ||
			    (neb_cmd_get_u32(image, slot_off +
					     NEB_CDP_FILL_PATTERN_SIZE) != 1 &&
			     neb_cmd_get_u32(image, slot_off +
					     NEB_CDP_FILL_PATTERN_SIZE) != 2 &&
			     neb_cmd_get_u32(image, slot_off +
					     NEB_CDP_FILL_PATTERN_SIZE) != 4))
				return -EINVAL;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_FILL_DST, fill_size,
					NEB_VM_PROT_WRITE, validation);
			if (ret)
				return ret;
			break;
		case NEB_CDP_PKT_DRAW_ARRAYS:
			expected_words = NEB_CDP_DRAW_ARRAYS_WORDS;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_ARRAYS_VERTEX, 1,
					NEB_VM_PROT_READ, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_ARRAYS_FB, 1,
					NEB_VM_PROT_WRITE, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_ARRAYS_DEPTH, 1,
					NEB_VM_PROT_WRITE, validation);
			if (ret)
				return ret;
			goto validate_draw_state;
		case NEB_CDP_PKT_DRAW_INDEXED:
			expected_words = NEB_CDP_DRAW_INDEXED_WORDS;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_INDEXED_VERTEX, 1,
					NEB_VM_PROT_READ, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_INDEXED_INDEX, 1,
					NEB_VM_PROT_READ, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_INDEXED_FB, 1,
					NEB_VM_PROT_WRITE, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_validate_leaf(image, image_size,
					old_base, new_base,
					slot_off + NEB_CDP_DRAW_INDEXED_DEPTH, 1,
					NEB_VM_PROT_WRITE, validation);
			if (ret)
				return ret;

validate_draw_state:
			ret = nebulae_submit_shadow_rebase_field(image,
					image_size, old_base, new_base,
					slot_off + NEB_CDP_DRAW_PIPELINE_STATE,
					&pipeline_va, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_shadow_rebase_field(image,
					image_size, old_base, new_base,
					slot_off + NEB_CDP_DRAW_DESCRIPTOR,
					&draw_desc_va, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_shadow_rebase_pipeline(image,
					image_size, old_base, new_base,
					pipeline_va, validation);
			if (ret)
				return ret;
			ret = nebulae_submit_shadow_rebase_draw_desc(image,
					image_size, old_base, new_base,
					draw_desc_va, validation);
			if (ret)
				return ret;
			break;
		default:
			return -EOPNOTSUPP;
		}

		if (payload_words != expected_words)
			return -EINVAL;
		if (memchr_inv(image + slot_off + sizeof(u64) +
			       payload_words * sizeof(u64), 0,
			       NEB_CDP_PACKET_SLOT_BYTES - sizeof(u64) -
			       payload_words * sizeof(u64)))
			return -EINVAL;
	}

	return 0;
}

static int nebulae_submit_shadow_alloc(struct nebulae_device *ndev,
				       u32 submit_size,
				       struct nebulae_submit_shadow *shadow)
{
	int ret;

	memset(shadow, 0, sizeof(*shadow));
	/* Keep executable command/shader bytes and the writable completion queue
	 * on distinct GPU pages.  The descriptor and privileged outer command are
	 * consumed by physical address and deliberately remain outside user GPUVA. */
	shadow->image_map_size = ALIGN((u64)submit_size, NEB_GPU_PAGE_SIZE);
	shadow->desc_rel = shadow->image_map_size;
	shadow->cmd_rel = ALIGN(shadow->desc_rel + NEB_QUEUE_DESC_SIZE,
				NEB_SUBMIT_META_ALIGN);
	shadow->cq_rel = ALIGN(shadow->cmd_rel + NEB_USER_CMD_SIZE,
			       NEB_GPU_PAGE_SIZE);
	shadow->total_size = ALIGN(shadow->cq_rel + NEB_CQ_ENTRY_SIZE,
				   NEB_GPU_PAGE_SIZE);

	mutex_lock(&ndev->bo_lock);
	ret = drm_mm_insert_node_generic(&ndev->vram_mm, &shadow->node,
					 shadow->total_size, NEB_GPU_PAGE_SIZE,
					 0, 0);
	mutex_unlock(&ndev->bo_lock);
	if (ret)
		return ret;

	shadow->phys = shadow->node.start;
	shadow->allocated = true;
	return 0;
}

static void nebulae_submit_shadow_free(struct nebulae_device *ndev,
				       struct nebulae_submit_shadow *shadow)
{
	if (!shadow->allocated)
		return;

	mutex_lock(&ndev->bo_lock);
	drm_mm_remove_node(&shadow->node);
	mutex_unlock(&ndev->bo_lock);
	shadow->allocated = false;
}

static int nebulae_hw_status_to_errno(u32 status)
{
	switch (status) {
	case NEB_HW_STATUS_OK:
		return 0;
	case NEB_HW_STATUS_INVALID_ARGUMENT:
		return -EINVAL;
	case NEB_HW_STATUS_OUT_OF_RANGE:
		return -ERANGE;
	case NEB_HW_STATUS_UNSUPPORTED:
		return -EOPNOTSUPP;
	case NEB_HW_STATUS_CANCELLED:
		return -ECANCELED;
	case NEB_HW_STATUS_KILLED:
	case NEB_HW_STATUS_FAULT:
	default:
		return -EIO;
	}
}

static int nebulae_hw_job_control_locked(struct nebulae_device *ndev, u64 seq,
					 u32 op, u32 *status)
{
	u32 control;

	if (!seq)
		return -EINVAL;
	if (!(ndev->hw_caps & NEB_CAP_JOB_CONTROL))
		return -EOPNOTSUPP;

	switch (op) {
	case DRM_NEBULAE_JOB_CONTROL_CANCEL:
		control = NEB_JOB_CONTROL_CANCEL;
		break;
	case DRM_NEBULAE_JOB_CONTROL_KILL:
		control = NEB_JOB_CONTROL_KILL;
		break;
	default:
		return -EINVAL;
	}

	neb_writeq(ndev, NEB_REG_JOB_CONTROL_SEQ_LO, seq);
	writel(control, ndev->regs + NEB_REG_JOB_CONTROL);
	if (status)
		*status = readl(ndev->regs + NEB_REG_LAST_ERROR);

	return 0;
}

static int nebulae_hw_job_control(struct nebulae_device *ndev, u64 seq,
				  u32 op, u32 *status)
{
	int ret;

	mutex_lock(&ndev->submit_lock);
	ret = nebulae_hw_job_control_locked(ndev, seq, op, status);
	mutex_unlock(&ndev->submit_lock);

	return ret;
}

static int nebulae_hw_submit_cmd_bo(struct nebulae_device *ndev,
				    struct nebulae_file *nfile,
				    struct nebulae_bo *cmd_bo,
				    u32 offset, u32 size, u32 cmd_count,
				    u64 pt_base, u32 asid, u64 cookie,
				    u64 *seq, u32 *hw_status,
				    struct nebulae_job *job)
{
	struct nebulae_submit_validation validation = { .job = job };
	struct drm_gem_object *obj = &cmd_bo->base.base;
	struct nebulae_submit_shadow *shadow;
	u8 *image;
	u8 cmd[NEB_USER_CMD_SIZE];
	u8 desc[NEB_QUEUE_DESC_SIZE];
	u64 source_va;
	u64 source_phys;
	u64 cq_va;
	u64 desc_phys;
	u64 cmd_phys;
	u64 cq_phys;
	int ret;

	if (!size || !cmd_count)
		return -EINVAL;
	nebulae_submit_snapshot_scanout(ndev, &validation);
	*seq = 0;
	*hw_status = 0;
	if ((u64)offset + size > obj->size)
		return -EINVAL;
	if (!cmd_bo->vram_offset)
		return -EINVAL;
	if (cmd_bo->vram_offset > ndev->vram_size ||
	    obj->size > ndev->vram_size - cmd_bo->vram_offset)
		return -EINVAL;
	ret = nebulae_vm_bo_va(nfile, cmd_bo, &source_va);
	if (ret)
		return ret;

	shadow = kzalloc(sizeof(*shadow), GFP_KERNEL);
	if (!shadow)
		return -ENOMEM;

	ret = nebulae_submit_shadow_alloc(ndev, size, shadow);
	if (ret)
		goto out_free_shadow_struct;

	image = kvmalloc(size, GFP_KERNEL);
	if (!image) {
		ret = -ENOMEM;
		goto out_free_shadow;
	}

	source_va += offset;
	source_phys = cmd_bo->vram_offset + offset;
	/* The validated immutable image carries packet state and embedded shader
	 * programs, so it is GPU-readable/executable but never GPU-writable. */
	ret = nebulae_vm_map_kernel(ndev, nfile, shadow->phys,
				    shadow->image_map_size,
				    NEB_VM_PROT_READ | NEB_VM_PROT_EXEC |
				    NEB_VM_PROT_USER, &shadow->image_vma);
	if (ret)
		goto out_free_image;
	shadow->image_va = shadow->image_vma->node.start;

	/* CP completion is the only GPU-written part of the shadow allocation.
	 * Give it a separate non-executable page to preserve W^X. */
	ret = nebulae_vm_map_kernel(ndev, nfile,
				    shadow->phys + shadow->cq_rel,
				    NEB_GPU_PAGE_SIZE,
				    NEB_VM_PROT_READ | NEB_VM_PROT_WRITE |
				    NEB_VM_PROT_USER, &shadow->cq_vma);
	if (ret)
		goto out_unmap_image;
	shadow->cq_va = shadow->cq_vma->node.start;

	memcpy_fromio(image, ndev->vram + source_phys, size);
	ret = nebulae_submit_shadow_rebase_packets(image, size, cmd_count,
						   source_va, shadow->image_va,
						   &validation);
	if (ret)
		goto out_free_image;
	memcpy_toio(ndev->vram + shadow->phys, image, size);

	cq_va = shadow->cq_va;
	desc_phys = shadow->phys + shadow->desc_rel;
	cmd_phys = shadow->phys + shadow->cmd_rel;
	cq_phys = shadow->phys + shadow->cq_rel;

	memset(desc, 0, sizeof(desc));
	neb_cmd_put_u32(desc, 0, NEB_QUEUE_DESC_MAGIC);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_VERSION_OFF,
			NEB_QUEUE_DESC_VERSION);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_QUEUE_ID, 1);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_RING_BASE, shadow->image_va);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_RING_SIZE, size);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_OFFSET, 0);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_SIZE, size);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_PACKET_COUNT, cmd_count);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_CQ_BASE, cq_va);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_CQ_SIZE, NEB_CQ_ENTRY_SIZE);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_CQ_ENTRY_SIZE, NEB_CQ_ENTRY_SIZE);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_COOKIE, cookie);
	neb_cmd_put_u64(desc, NEB_QUEUE_DESC_PT_BASE, pt_base);
	neb_cmd_put_u32(desc, NEB_QUEUE_DESC_ASID, asid);

	memset(cmd, 0, sizeof(cmd));
	neb_cmd_put_u32(cmd, 0, NEB_USER_CMD_CP_EXEC);
	/* The privileged outer command is consumed before ASID/PTBR is bound. */
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_BASE, desc_phys);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_RING_SIZE, sizeof(desc));
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_OFFSET, 0);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_SIZE, sizeof(desc));
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_PACKET_COUNT, 1);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_COOKIE, cookie);
	neb_cmd_put_u64(cmd, NEB_USER_CMD_CP_PT_BASE, pt_base);
	neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_ASID, asid);

	mutex_lock(&ndev->submit_lock);
	/* File close and hardware publication share submit_lock.  Once closing is
	 * visible, a queued scheduler job must not cross the active-job boundary:
	 * postclose either observes an already-published job and kills it, or this
	 * check rejects the job before it can reach the doorbell. */
	if (READ_ONCE(nfile->closing)) {
		mutex_unlock(&ndev->submit_lock);
		ret = -ECANCELED;
		goto out_free_image;
	}
	if (READ_ONCE(ndev->resetting)) {
		mutex_unlock(&ndev->submit_lock);
		ret = -EAGAIN;
		goto out_free_image;
	}
	if (READ_ONCE(ndev->suspended) || READ_ONCE(ndev->wedged)) {
		mutex_unlock(&ndev->submit_lock);
		ret = -ENODEV;
		goto out_free_image;
	}
	ndev->last_submit_cookie = cookie;
	ndev->last_submit_pt_base = pt_base;
	ndev->last_submit_asid = asid;
	memset_io(ndev->vram + cq_phys, 0, NEB_CQ_ENTRY_SIZE);
	memcpy_toio(ndev->vram + desc_phys, desc, sizeof(desc));

	spin_lock_irq(&ndev->hw_lock);
	if (ndev->active_job) {
		spin_unlock_irq(&ndev->hw_lock);
		mutex_unlock(&ndev->submit_lock);
		ret = -EBUSY;
		goto out_free_image;
	}
	job->shadow = shadow;
	job->cq_phys = cq_phys;
	nebulae_job_get(job); /* active-job reference, dropped by IRQ/reset */
	ndev->active_job = job;
	spin_unlock_irq(&ndev->hw_lock);

	/* Serialize the final ownership classification with KMS register
	 * programming through the doorbell.  If the plane changed while packet
	 * validation ran, upgrade conservatively rather than risk a missed writer. */
	spin_lock_irq(&ndev->scanout_lock);
	if (validation.scanout_generation != ndev->scanout_generation)
		validation.may_write_scanout = true;
	if (ndev->hw_caps & NEB_CAP_CP_RESOURCE_OWNERSHIP)
		neb_cmd_put_u32(cmd, NEB_USER_CMD_CP_FLAGS,
				NEB_USER_CMD_CP_FLAG_WRITESET_VALID |
				(validation.may_write_scanout ?
				 NEB_USER_CMD_CP_FLAG_MAY_WRITE_SCANOUT : 0));
	memcpy_toio(ndev->vram + cmd_phys, cmd, sizeof(cmd));

	writel(NEB_IRQ_ALL, ndev->regs + NEB_REG_IRQ_STATUS);
	neb_writeq(ndev, NEB_REG_SUBMIT_OFFSET_LO, cmd_phys);
	writel(sizeof(cmd), ndev->regs + NEB_REG_SUBMIT_SIZE);
	/* The immutable shadow contains the executable shader image for this job.
	 * Physical storage, ASID/PTBR and GPUVA may all be reused after an earlier
	 * client or submission is retired, so a TLB shootdown alone cannot make a
	 * decoded instruction cache coherent.  Publish the bytes, request the
	 * architecturally separate I-cache invalidate, then ring the doorbell. */
	wmb();
	writel(NEB_ICACHE_INVALIDATE_ALL,
	       ndev->regs + NEB_REG_ICACHE_INVALIDATE);
	wmb();
	writel(1, ndev->regs + NEB_REG_DOORBELL);
	spin_unlock_irq(&ndev->scanout_lock);

	*seq = neb_readq(ndev, NEB_REG_JOB_SEQ_LO);
	WRITE_ONCE(job->hw_seq, *seq);
	atomic64_set(&ndev->submitted_jobs, *seq);
	trace_nebulae_job("run", job->nfile->ctx_id, job->user_seq,
			   job->hw_seq, job->asid, 0);
	mutex_unlock(&ndev->submit_lock);
	kvfree(image);
	return 0;

out_free_image:
	if (shadow->cq_vma) {
		nebulae_vm_unmap_kernel(ndev, nfile, shadow->cq_vma);
		shadow->cq_vma = NULL;
	}
out_unmap_image:
	if (shadow->image_vma) {
		nebulae_vm_unmap_kernel(ndev, nfile, shadow->image_vma);
		shadow->image_vma = NULL;
	}
	kvfree(image);
out_free_shadow:
	nebulae_submit_shadow_free(ndev, shadow);
out_free_shadow_struct:
	kfree(shadow);
	return ret;
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

static void nebulae_job_release_shadow(struct nebulae_job *job)
{
	struct nebulae_submit_shadow *shadow = job->shadow;

	if (!shadow)
		return;
	job->shadow = NULL;
	nebulae_vm_unmap_kernel(job->ndev, job->nfile, shadow->cq_vma);
	nebulae_vm_unmap_kernel(job->ndev, job->nfile, shadow->image_vma);
	nebulae_submit_shadow_free(job->ndev, shadow);
	kfree(shadow);
}

static void nebulae_job_pm_put(struct nebulae_job *job)
{
	if (!job->pm_ref)
		return;
	job->pm_ref = false;
	pm_runtime_mark_last_busy(job->ndev->drm.dev);
	pm_runtime_put_autosuspend(job->ndev->drm.dev);
}

static u32 nebulae_fault_reason(u32 hw_status)
{
	switch (hw_status) {
	case NEB_HW_STATUS_INVALID_ARGUMENT:
	case NEB_HW_STATUS_OUT_OF_RANGE:
	case NEB_HW_STATUS_UNSUPPORTED:
		return DRM_NEBULAE_FAULT_ILLEGAL_PACKET;
	case NEB_HW_STATUS_FAULT:
		return DRM_NEBULAE_FAULT_BUS;
	default:
		return DRM_NEBULAE_FAULT_UNKNOWN;
	}
}

void nebulae_submit_abort_active(struct nebulae_device *ndev, int error,
				 u32 reason)
{
	struct nebulae_job *job;

	spin_lock_irq(&ndev->hw_lock);
	job = ndev->active_job;
	ndev->active_job = NULL;
	spin_unlock_irq(&ndev->hw_lock);
	if (!job)
		return;

	job->result = error;
	nebulae_fault_record(ndev, job, reason, 0,
			      DRM_NEBULAE_FAULT_FLAG_RESET_REQUIRED, 0,
			      job->hw_status, error);
	nebulae_job_release_shadow(job);
	nebulae_job_pm_put(job);
	if (error)
		dma_fence_set_error(job->hw_fence, error);
	if (dma_fence_signal(job->hw_fence)) {
		atomic64_inc(&ndev->signaled_fences);
		nebulae_sched_record_complete(ndev, error);
	}
	wake_up_all(&ndev->submit_wait);
	nebulae_job_put(job);
}

void nebulae_submit_irq_process(struct nebulae_device *ndev, u32 irq_status)
{
	struct nebulae_job *job;
	u8 cq[NEB_CQ_ENTRY_SIZE];
	u64 completed;
	u64 cq_seq;
	u32 cq_status;
	u32 fault_reason = DRM_NEBULAE_FAULT_UNKNOWN;
	u32 fault_flags = 0;
	bool reset_required = false;
	int ret = 0;

	spin_lock_irq(&ndev->hw_lock);
	job = ndev->active_job;
	ndev->active_job = NULL;
	spin_unlock_irq(&ndev->hw_lock);
	if (!job) {
		if (irq_status & NEB_IRQ_FAULT)
			nebulae_schedule_reset(ndev, DRM_NEBULAE_FAULT_UNKNOWN);
		return;
	}

	memcpy_fromio(cq, ndev->vram + job->cq_phys, sizeof(cq));
	if (neb_cmd_get_u32(cq, 0) != NEB_CQ_ENTRY_MAGIC ||
	    neb_cmd_get_u64(cq, NEB_CQ_COOKIE) != job->cookie) {
		ret = -EPROTO;
		cq_status = readl(ndev->regs + NEB_REG_LAST_ERROR);
		reset_required = true;
	} else {
		cq_status = neb_cmd_get_u32(cq, NEB_CQ_STATUS);
		cq_seq = neb_cmd_get_u64(cq, NEB_CQ_HW_SEQ);
		completed = neb_cmd_get_u64(cq, NEB_CQ_COMPLETED_SEQ);
		if (cq_seq)
			WRITE_ONCE(job->hw_seq, cq_seq);
		atomic64_set(&ndev->completed_jobs, completed);
		if (cq_status)
			ret = nebulae_hw_status_to_errno(cq_status);
	}

	if ((irq_status & NEB_IRQ_FAULT) && !ret)
		ret = -EIO;
	if (ret && cq_status != NEB_HW_STATUS_CANCELLED &&
	    cq_status != NEB_HW_STATUS_KILLED) {
		fault_reason = nebulae_fault_reason(cq_status);
		if (cq_status == NEB_HW_STATUS_FAULT)
			reset_required = true;
		if (reset_required)
			fault_flags |= DRM_NEBULAE_FAULT_FLAG_RESET_REQUIRED;
		nebulae_fault_record(ndev, job, fault_reason, 0, fault_flags, 0,
				      cq_status, ret);
	}
	job->hw_status = cq_status;
	job->result = ret;
	WRITE_ONCE(ndev->last_error, cq_status);
	trace_nebulae_job("complete", job->nfile->ctx_id, job->user_seq,
			   job->hw_seq, job->asid, ret);

	/* Release DMA-visible control memory before publishing completion. */
	nebulae_job_release_shadow(job);
	nebulae_job_pm_put(job);
	if (ret)
		dma_fence_set_error(job->hw_fence, ret);
	dma_fence_signal(job->hw_fence);
	atomic64_inc(&ndev->signaled_fences);
	nebulae_sched_record_complete(ndev, ret);
	wake_up_all(&ndev->submit_wait);
	nebulae_job_put(job); /* active-job reference */
	if (reset_required)
		nebulae_schedule_reset(ndev, fault_reason);
}

static int nebulae_job_register(struct nebulae_job *job)
{
	int ret;

	if (job->user_seq > ULONG_MAX)
		return -EOVERFLOW;

	nebulae_job_get(job);
	ret = xa_insert(&job->nfile->jobs, (unsigned long)job->user_seq, job,
			GFP_KERNEL);
	if (ret) {
		nebulae_job_put(job);
		return ret;
	}
	job->job_registered = true;
	return 0;
}

static void nebulae_job_unregister(struct nebulae_job *job)
{
	void *entry;

	if (!job->job_registered)
		return;

	entry = xa_erase(&job->nfile->jobs, (unsigned long)job->user_seq);
	job->job_registered = false;
	if (WARN_ON(entry != job))
		return;
	nebulae_job_put(job);
	wake_up_all(&job->ndev->submit_wait);
}

void nebulae_submit_file_kill_active(struct nebulae_device *ndev,
				     struct nebulae_file *nfile)
{
	struct nebulae_job *job = NULL;
	u32 status = 0;

	/* Serialize against the final closing check and active-job publication in
	 * nebulae_hw_submit_cmd_bo().  Without this handshake a scheduler worker
	 * can publish immediately after postclose observes no active job, escaping
	 * KILL and retaining the single hardware queue behind a dead client. */
	mutex_lock(&ndev->submit_lock);
	spin_lock_irq(&ndev->hw_lock);
	if (ndev->active_job && ndev->active_job->nfile == nfile) {
		job = ndev->active_job;
		nebulae_job_get(job);
	}
	spin_unlock_irq(&ndev->hw_lock);
	if (job && READ_ONCE(job->hw_seq) &&
	    !dma_fence_is_signaled(&job->base.s_fence->finished))
		(void)nebulae_hw_job_control_locked(
			ndev, job->hw_seq, DRM_NEBULAE_JOB_CONTROL_KILL, &status);
	mutex_unlock(&ndev->submit_lock);
	if (job)
		nebulae_job_put(job);
}

static struct nebulae_job *nebulae_job_lookup_owned(struct nebulae_file *nfile,
						     u64 user_seq)
{
	struct nebulae_job *job;

	if (!user_seq || user_seq > ULONG_MAX)
		return NULL;

	xa_lock(&nfile->jobs);
	job = xa_load(&nfile->jobs, (unsigned long)user_seq);
	if (job && !kref_get_unless_zero(&job->refcount))
		job = NULL;
	xa_unlock(&nfile->jobs);

	return job;
}

static void nebulae_job_put_bo_refs(struct nebulae_job *job)
{
	u32 i;

	if (!job)
		return;

	if (job->cmd_obj) {
		drm_gem_object_put(job->cmd_obj);
		job->cmd_obj = NULL;
		job->cmd_bo = NULL;
	}

	if (!job->objs)
		return;

	for (i = 0; i < job->obj_count; i++)
		drm_gem_object_put(job->objs[i]);
	kvfree(job->objs);
	job->objs = NULL;
	job->obj_count = 0;
}

static int nebulae_job_lookup_bos(struct drm_file *file,
				  struct drm_nebulae_submit_cmd_bo *args,
				  struct nebulae_job *job)
{
	struct drm_gem_object **lookup = NULL;
	struct drm_gem_object **objs;
	u32 count = 1;
	u32 i, j;
	int ret;

	if ((!args->bo_handles && args->bo_handle_count) ||
	    args->bo_handle_count > NEB_SUBMIT_MAX_BOS)
		return -EINVAL;

	if (args->bo_handle_count) {
		ret = drm_gem_objects_lookup(file,
					     (void __user *)(uintptr_t)args->bo_handles,
					     args->bo_handle_count, &lookup);
		if (ret)
			return ret;
	}

	objs = kvmalloc_array(args->bo_handle_count + 1, sizeof(*objs),
			      GFP_KERNEL);
	if (!objs) {
		ret = -ENOMEM;
		goto out_put_lookup;
	}

	/* The command BO participates in reservation locking even when userspace
	 * omitted it from the resource list.  This closes submit-vs-VM_UNMAP and
	 * submit-vs-CPU-write races. */
	objs[0] = job->cmd_obj;
	drm_gem_object_get(objs[0]);
	for (i = 0; i < args->bo_handle_count; i++) {
		bool duplicate = false;

		for (j = 0; j < count; j++) {
			if (objs[j] == lookup[i]) {
				duplicate = true;
				break;
			}
		}
		if (duplicate)
			drm_gem_object_put(lookup[i]);
		else
			objs[count++] = lookup[i];
	}
	kvfree(lookup);

	job->objs = objs;
	job->obj_count = count;
	return 0;

out_put_lookup:
	for (i = 0; i < args->bo_handle_count; i++)
		drm_gem_object_put(lookup[i]);
	kvfree(lookup);
	return ret;
}

static int nebulae_job_lock_reservations(struct nebulae_job *job,
					 struct ww_acquire_ctx *acquire_ctx)
{
	u32 i;
	int ret;

	if (!job->obj_count)
		return 0;

	ret = drm_gem_lock_reservations(job->objs, job->obj_count,
					acquire_ctx);
	if (ret)
		return ret;

	for (i = 0; i < job->obj_count; i++) {
		ret = dma_resv_reserve_fences(job->objs[i]->resv, 1);
		if (ret)
			goto fail_unlock;

		ret = drm_sched_job_add_implicit_dependencies(&job->base,
							      job->objs[i],
							      true);
		if (ret)
			goto fail_unlock;
	}

	return 0;

fail_unlock:
	drm_gem_unlock_reservations(job->objs, job->obj_count, acquire_ctx);
	return ret;
}

static int nebulae_job_sync_bos_to_vram(struct nebulae_job *job)
{
	u32 i;
	int ret;

	for (i = 0; i < job->obj_count; i++) {
		struct nebulae_bo *bo = to_nebulae_bo(job->objs[i]);

		/* Preserve device-produced data across dependent GPU jobs.  CPU
		 * ownership is explicit through BO_SET_DOMAIN; only CPU-dirty BOs
		 * are uploaded from the authoritative shmem backing. */
		if (bo != job->cmd_bo &&
		    !(READ_ONCE(bo->domain) & DRM_NEBULAE_BO_DOMAIN_CPU))
			continue;
		/* Scheduler dependencies already waited prior reservation fences.
		 * Waiting again here would include this job's own finished fence. */
		ret = nebulae_bo_sync_to_vram_nowait(job->ndev,
						     bo);
		if (ret)
			return ret;
	}
	return 0;
}

static void nebulae_job_attach_fences_and_unlock(struct nebulae_job *job,
						 struct dma_fence *fence,
						 struct ww_acquire_ctx *acquire_ctx)
{
	u32 i;

	if (!job->obj_count)
		return;

	for (i = 0; i < job->obj_count; i++)
		dma_resv_add_fence(job->objs[i]->resv, fence,
				   DMA_RESV_USAGE_WRITE);

	drm_gem_unlock_reservations(job->objs, job->obj_count, acquire_ctx);
}

static struct dma_fence *nebulae_job_run(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);
	struct nebulae_device *ndev = job->ndev;
	struct dma_fence *fence;
	bool entered = false;
	int idx;
	int ret;

	if (unlikely(job->base.s_fence->finished.error)) {
		job->result = job->base.s_fence->finished.error;
		nebulae_sched_record_complete(ndev, job->result);
		return NULL;
	}

	fence = nebulae_fence_create(ndev);
	if (!fence) {
		job->result = -ENOMEM;
		nebulae_sched_record_complete(ndev, job->result);
		return ERR_PTR(-ENOMEM);
	}
	job->hw_fence = fence;

	ret = pm_runtime_resume_and_get(ndev->drm.dev);
	if (ret >= 0) {
		job->pm_ref = true;
		ret = 0;
	}
	if (!ret && READ_ONCE(ndev->resetting))
		ret = -EAGAIN;
	else if (!ret && (READ_ONCE(ndev->suspended) ||
			READ_ONCE(ndev->wedged)))
		ret = -ENODEV;
	else if (!ret && !drm_dev_enter(&ndev->drm, &idx))
		ret = -ENODEV;
	else if (!ret) {
		entered = true;
		ret = nebulae_job_sync_bos_to_vram(job);
	}
	if (!ret)
		ret = nebulae_hw_submit_cmd_bo(ndev, job->nfile,
					       job->cmd_bo, job->offset,
					       job->size, job->cmd_count,
					       job->pt_base, job->asid,
					       job->cookie,
					       &job->hw_seq,
					       &job->hw_status, job);
	if (entered)
		drm_dev_exit(idx);

	job->result = ret;
	if (job->result) {
		nebulae_job_pm_put(job);
		if (ret == -EINVAL || ret == -EACCES || ret == -EOPNOTSUPP ||
		    ret == -EPROTO)
			nebulae_fault_record(ndev, job,
					      DRM_NEBULAE_FAULT_ILLEGAL_PACKET,
					      0, 0, 0, job->hw_status, ret);
		dma_fence_set_error(fence, job->result);
		dma_fence_signal(fence);
		atomic64_inc(&ndev->signaled_fences);
		nebulae_sched_record_complete(ndev, job->result);
	}

	/* The scheduler owns this reference; the job keeps the original until
	 * free_job.  Successful jobs are signaled only by IRQ/reset. */
	return dma_fence_get(fence);
}

static enum drm_gpu_sched_stat
nebulae_job_timedout(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);
	struct nebulae_device *ndev = job->ndev;

	WRITE_ONCE(ndev->last_error, ETIMEDOUT);
	job->result = -ETIMEDOUT;
	nebulae_device_reset(ndev, sched_job, DRM_NEBULAE_FAULT_TIMEOUT);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void nebulae_job_free(struct drm_sched_job *sched_job)
{
	struct nebulae_job *job = container_of(sched_job, struct nebulae_job,
					      base);

	if (WARN_ON(job->shadow))
		nebulae_job_release_shadow(job);
	nebulae_job_pm_put(job);
	dma_fence_put(job->hw_fence);
	job->hw_fence = NULL;
	nebulae_job_unregister(job);
	drm_sched_job_cleanup(sched_job);
	nebulae_vm_job_unpin_bos(job->ndev, job->nfile, &job->vmas,
				  &job->vma_count);
	nebulae_job_put_bo_refs(job);
	if (job->nfile_ref) {
		job->nfile_ref = false;
		nebulae_file_put(job->nfile);
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
	struct drm_gem_object *obj = NULL;
	struct nebulae_bo *bo;
	struct nebulae_job *job;
	struct dma_fence *finished = NULL;
	struct ww_acquire_ctx acquire_ctx;
	u32 supported_flags = DRM_NEBULAE_SUBMIT_ASYNC |
			      DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ |
			      DRM_NEBULAE_SUBMIT_IN_FENCE_FD |
			      DRM_NEBULAE_SUBMIT_OUT_FENCE_FD;
	u32 status = 0;
	u64 seq = 0;
	bool bos_locked = false;
	bool submit_locked = false;
	int export_ret = 0;
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

	ret = mutex_lock_interruptible(&nfile->submit_lock);
	if (ret)
		return ret;
	submit_locked = true;
	if (READ_ONCE(nfile->closing)) {
		ret = -ENODEV;
		goto out_put;
	}

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj) {
		ret = -ENOENT;
		goto out_put;
	}

	if ((u64)args->offset + args->size > obj->size) {
		ret = -EINVAL;
		args->driver_error = ret;
		goto out_put;
	}

	bo = to_nebulae_bo(obj);
	if ((bo->flags & DRM_NEBULAE_BO_TYPE_MASK) !=
	    DRM_NEBULAE_BO_TYPE_COMMAND) {
		ret = -EINVAL;
		args->driver_error = ret;
		goto out_put;
	}

	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job) {
		ret = -ENOMEM;
		goto out_put;
	}

	kref_init(&job->refcount);
	job->ndev = ndev;
	if (!nebulae_file_get(nfile)) {
		ret = -ENODEV;
		goto out_free_job;
	}
	job->nfile = nfile;
	job->nfile_ref = true;
	job->cmd_obj = obj;
	job->cmd_bo = bo;
	obj = NULL;
	job->offset = args->offset;
	job->size = args->size;
	job->cmd_count = args->cmd_count;

	ret = nebulae_job_lookup_bos(file, args, job);
	if (ret)
		goto out_free_job;

	ret = nebulae_vm_job_pin_bos(nfile, job->objs, job->obj_count,
				      &job->vmas, &job->vma_count);
	if (ret)
		goto out_free_job;

	ret = drm_sched_job_init(&job->base, &nfile->sched_entity, 1, nfile);
	if (ret)
		goto out_free_job;

	ret = nebulae_job_add_deps(file, args, job);
	if (ret)
		goto out_cleanup_job;

	ret = nebulae_job_lock_reservations(job, &acquire_ctx);
	if (ret)
		goto out_cleanup_job;
	bos_locked = job->obj_count != 0;

	job->user_seq = atomic64_inc_return(&nfile->next_job_seq);
	seq = job->user_seq;
	job->cookie = job->user_seq;
	job->asid = nfile->asid;
	job->pt_base = nfile->mmu_root;
	ret = nebulae_job_register(job);
	if (ret)
		goto out_cleanup_job;

	/* drm_sched_job_arm() is the point of no return.  Every allocation,
	 * VMA pin and xarray insertion is complete; from here to entity_push_job()
	 * the per-context lock preserves the entity's SPSC ordering contract. */
	drm_sched_job_arm(&job->base);
	finished = dma_fence_get(&job->base.s_fence->finished);
	/* Fence export can still fail while allocating a sync_file.  The armed job
	 * must nevertheless be committed and freed by the scheduler; report the
	 * export error after push instead of illegally cleaning up an armed job. */
	export_ret = nebulae_export_out_fence(file, args, finished);

	if (bos_locked) {
		nebulae_job_attach_fences_and_unlock(job, finished,
						     &acquire_ctx);
		bos_locked = false;
	}

	nebulae_job_get(job);
	atomic64_inc(&nfile->submits);
	nebulae_sched_record_submit(ndev);
	drm_sched_entity_push_job(&job->base);
	trace_nebulae_job("submit", nfile->ctx_id, job->user_seq, 0,
			   job->asid, 0);
	mutex_unlock(&nfile->submit_lock);
	submit_locked = false;
	if (export_ret) {
		ret = export_ret;
		goto out_committed;
	}

	if (!(args->flags & DRM_NEBULAE_SUBMIT_ASYNC)) {
		ret = nebulae_wait_fence(finished, args->timeout_ns);
		if (!ret)
			ret = job->result;

		status = job->hw_status;
	}

out_committed:
	args->seq = seq;
	args->status = status;
	args->driver_error = ret;
	dma_fence_put(finished);
	nebulae_job_put(job);
	return ret;

out_cleanup_job:
	nebulae_job_unregister(job);
	if (bos_locked)
		drm_gem_unlock_reservations(job->objs, job->obj_count,
					    &acquire_ctx);
	dma_fence_put(finished);
	drm_sched_job_cleanup(&job->base);
out_free_job:
	nebulae_vm_job_unpin_bos(ndev, nfile, &job->vmas, &job->vma_count);
	nebulae_job_put_bo_refs(job);
	if (job->nfile_ref) {
		job->nfile_ref = false;
		nebulae_file_put(nfile);
	}
	kfree(job);
out_put:
	if (obj)
		drm_gem_object_put(obj);
	if (submit_locked)
		mutex_unlock(&nfile->submit_lock);
	return ret;
}

int nebulae_ioctl_job_control(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct nebulae_device *ndev = to_nebulae(drm);
	struct nebulae_file *nfile = file->driver_priv;
	struct drm_nebulae_job_control *args = data;
	struct nebulae_job *job;
	int idx;
	int ret;

	if (!nfile)
		return -EINVAL;
	if (args->flags)
		return -EINVAL;
	if (args->op != DRM_NEBULAE_JOB_CONTROL_CANCEL &&
	    args->op != DRM_NEBULAE_JOB_CONTROL_KILL)
		return -EINVAL;

	job = nebulae_job_lookup_owned(nfile, args->seq);
	if (!job) {
		ret = -ENOENT;
		goto out;
	}

	if (!READ_ONCE(job->hw_seq)) {
		ret = -EAGAIN;
	} else if (dma_fence_is_signaled(&job->base.s_fence->finished)) {
		ret = -EALREADY;
	} else {
		ret = nebulae_device_enter(ndev, &idx);
		if (!ret) {
			ret = nebulae_hw_job_control(ndev, job->hw_seq, args->op,
						     &args->status);
			nebulae_device_exit(ndev, idx);
		}
	}
	nebulae_job_put(job);

out:
	args->driver_error = ret;
	if (!ret)
		WRITE_ONCE(ndev->last_error, args->status);

	return ret;
}
