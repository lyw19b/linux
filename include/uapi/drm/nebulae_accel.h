/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * UAPI for the Nebulae LAXPU simulator accel driver.
 */

#ifndef __UAPI_NEBULAE_ACCEL_H__
#define __UAPI_NEBULAE_ACCEL_H__

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

#define DRM_NEBULAE_DRIVER_MAJOR 0
#define DRM_NEBULAE_DRIVER_MINOR 8

#define DRM_NEBULAE_GET_PARAM	0x00
#define DRM_NEBULAE_BO_CREATE	0x01
#define DRM_NEBULAE_BO_INFO	0x02
#define DRM_NEBULAE_MMAP_BO	0x03
#define DRM_NEBULAE_VM_BIND	0x04
#define DRM_NEBULAE_VM_UNBIND	0x05
#define DRM_NEBULAE_SUBMIT	0x06
#define DRM_NEBULAE_JOB_WAIT	0x07
#define DRM_NEBULAE_VM_INFO	0x08
#define DRM_NEBULAE_BO_SYNC	0x09
#define DRM_NEBULAE_RESET	0x0a
#define DRM_NEBULAE_HWCTX_CREATE 0x0b
#define DRM_NEBULAE_HWCTX_DESTROY 0x0c
#define DRM_NEBULAE_HWCTX_INFO	0x0d
#define DRM_NEBULAE_SUBMIT_EX	0x0e
#define DRM_NEBULAE_HWCTX_CONFIG 0x0f
#define DRM_NEBULAE_EXEC_CMD	0x10
#define DRM_NEBULAE_GET_INFO	0x11
#define DRM_NEBULAE_SET_STATE	0x12
#define DRM_NEBULAE_GET_ARRAY	0x13
#define DRM_NEBULAE_VM_BIND_EX	0x14
#define DRM_NEBULAE_VM_UNBIND_EX 0x15
#define DRM_NEBULAE_SUBMIT_CMD_BO 0x16

#define DRM_IOCTL_NEBULAE_GET_PARAM \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_PARAM, \
		 struct drm_nebulae_param)
#define DRM_IOCTL_NEBULAE_BO_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_CREATE, \
		 struct drm_nebulae_bo_create)
#define DRM_IOCTL_NEBULAE_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_BO_INFO, \
		 struct drm_nebulae_bo_info)
#define DRM_IOCTL_NEBULAE_MMAP_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_MMAP_BO, \
		 struct drm_nebulae_mmap_bo)
#define DRM_IOCTL_NEBULAE_VM_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_VM_BIND, \
		 struct drm_nebulae_vm_bind)
#define DRM_IOCTL_NEBULAE_VM_UNBIND \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NEBULAE_VM_UNBIND, \
		struct drm_nebulae_vm_unbind)
#define DRM_IOCTL_NEBULAE_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_SUBMIT, \
		 struct drm_nebulae_submit)
#define DRM_IOCTL_NEBULAE_JOB_WAIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_JOB_WAIT, \
		 struct drm_nebulae_job_wait)
#define DRM_IOCTL_NEBULAE_VM_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_VM_INFO, \
		 struct drm_nebulae_vm_info)
#define DRM_IOCTL_NEBULAE_BO_SYNC \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NEBULAE_BO_SYNC, \
		struct drm_nebulae_bo_sync)
#define DRM_IOCTL_NEBULAE_RESET \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_RESET, \
		 struct drm_nebulae_reset)
#define DRM_IOCTL_NEBULAE_HWCTX_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_HWCTX_CREATE, \
		 struct drm_nebulae_hwctx_create)
#define DRM_IOCTL_NEBULAE_HWCTX_DESTROY \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NEBULAE_HWCTX_DESTROY, \
		struct drm_nebulae_hwctx_destroy)
#define DRM_IOCTL_NEBULAE_HWCTX_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_HWCTX_INFO, \
		 struct drm_nebulae_hwctx_info)
#define DRM_IOCTL_NEBULAE_SUBMIT_EX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_SUBMIT_EX, \
		 struct drm_nebulae_submit_ex)
#define DRM_IOCTL_NEBULAE_HWCTX_CONFIG \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_HWCTX_CONFIG, \
		 struct drm_nebulae_hwctx_config)
#define DRM_IOCTL_NEBULAE_EXEC_CMD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_EXEC_CMD, \
		 struct drm_nebulae_exec_cmd)
#define DRM_IOCTL_NEBULAE_GET_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_INFO, \
		 struct drm_nebulae_get_info)
#define DRM_IOCTL_NEBULAE_SET_STATE \
	DRM_IOW(DRM_COMMAND_BASE + DRM_NEBULAE_SET_STATE, \
		struct drm_nebulae_set_state)
#define DRM_IOCTL_NEBULAE_GET_ARRAY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_GET_ARRAY, \
		 struct drm_nebulae_get_array)
#define DRM_IOCTL_NEBULAE_VM_BIND_EX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_VM_BIND_EX, \
		 struct drm_nebulae_vm_bind_ex)
#define DRM_IOCTL_NEBULAE_VM_UNBIND_EX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_VM_UNBIND_EX, \
		 struct drm_nebulae_vm_unbind_ex)
#define DRM_IOCTL_NEBULAE_SUBMIT_CMD_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_NEBULAE_SUBMIT_CMD_BO, \
		 struct drm_nebulae_submit_cmd_bo)

#define DRM_NEBULAE_PARAM_DEVICE_ID		0
#define DRM_NEBULAE_PARAM_VERSION		1
#define DRM_NEBULAE_PARAM_VRAM_SIZE		2
#define DRM_NEBULAE_PARAM_CONTEXT_ID		3
#define DRM_NEBULAE_PARAM_CAPS			4
#define DRM_NEBULAE_PARAM_JOB_SEQ		5
#define DRM_NEBULAE_PARAM_COMPLETED_SEQ		6
#define DRM_NEBULAE_PARAM_LAST_ERROR		7
#define DRM_NEBULAE_PARAM_VM_START		8
#define DRM_NEBULAE_PARAM_VM_SIZE		9
#define DRM_NEBULAE_PARAM_PAGE_SIZE		10
#define DRM_NEBULAE_PARAM_PT_BASE		11
#define DRM_NEBULAE_PARAM_PT_SIZE		12
#define DRM_NEBULAE_PARAM_CMD_BASE		13
#define DRM_NEBULAE_PARAM_SUBMITTED_JOBS	14
#define DRM_NEBULAE_PARAM_FAULTED_JOBS		15
#define DRM_NEBULAE_PARAM_HWCTX_COUNT		16
#define DRM_NEBULAE_PARAM_CP_SUBMITTED_COOKIE	17
#define DRM_NEBULAE_PARAM_CP_COMPLETED_COOKIE	18

#define DRM_NEBULAE_CAP_NDRANGE			(1ULL << 0)
#define DRM_NEBULAE_CAP_IRQ			(1ULL << 1)
#define DRM_NEBULAE_CAP_VM_BIND			(1ULL << 2)
#define DRM_NEBULAE_CAP_SHMEM_BO		(1ULL << 3)
#define DRM_NEBULAE_CAP_SHADOW_MMU		(1ULL << 4)
#define DRM_NEBULAE_CAP_BO_SYNC			(1ULL << 5)
#define DRM_NEBULAE_CAP_RESET			(1ULL << 6)
#define DRM_NEBULAE_CAP_PARTIAL_BIND		(1ULL << 7)
#define DRM_NEBULAE_CAP_ASYNC_SUBMIT		(1ULL << 8)
#define DRM_NEBULAE_CAP_SYNCOBJ			(1ULL << 9)
#define DRM_NEBULAE_CAP_HWCTX			(1ULL << 10)
#define DRM_NEBULAE_CAP_SUBMIT_EX		(1ULL << 11)
#define DRM_NEBULAE_CAP_DRM_SCHED		(1ULL << 12)
#define DRM_NEBULAE_CAP_HWCTX_CONFIG		(1ULL << 13)
#define DRM_NEBULAE_CAP_EXEC_CMD		(1ULL << 14)
#define DRM_NEBULAE_CAP_INFO_QUERY		(1ULL << 15)
#define DRM_NEBULAE_CAP_STATE_CONTROL		(1ULL << 16)
#define DRM_NEBULAE_CAP_RESOURCE_QUERY		(1ULL << 17)
#define DRM_NEBULAE_CAP_TELEMETRY_QUERY		(1ULL << 18)
#define DRM_NEBULAE_CAP_POWER_STATE		(1ULL << 19)
#define DRM_NEBULAE_CAP_FW_VERSION		(1ULL << 20)
#define DRM_NEBULAE_CAP_CTX_HEALTH		(1ULL << 21)
#define DRM_NEBULAE_CAP_BO_PLACEMENT		(1ULL << 22)
#define DRM_NEBULAE_CAP_CMD_BO			(1ULL << 23)
#define DRM_NEBULAE_CAP_QOS_PRIORITY		(1ULL << 24)
#define DRM_NEBULAE_CAP_EXEC_SYNCOBJ_DEPENDENCY (1ULL << 25)
#define DRM_NEBULAE_CAP_EXEC_SYNCOBJ_SIGNAL	(1ULL << 26)
#define DRM_NEBULAE_CAP_FENCE_FD		(1ULL << 27)
#define DRM_NEBULAE_CAP_TLB_INVALIDATE		(1ULL << 28)
#define DRM_NEBULAE_CAP_FAULT_REPORT		(1ULL << 29)
#define DRM_NEBULAE_CAP_VM_BIND_FENCE		(1ULL << 30)
#define DRM_NEBULAE_CAP_CMD_BO_SUBMIT		(1ULL << 31)
#define DRM_NEBULAE_CAP_CP_QUEUE		(1ULL << 32)

#define DRM_NEBULAE_BO_WC			(1U << 0)
#define DRM_NEBULAE_BO_NO_AUTO_BIND		(1U << 1)
#define DRM_NEBULAE_BO_PLACEMENT_SHIFT		16
#define DRM_NEBULAE_BO_PLACEMENT_MASK		(0xffU << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_SHMEM		(1U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_DEVICE_LOCAL	(2U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_PLACEMENT_SHARED		(3U << DRM_NEBULAE_BO_PLACEMENT_SHIFT)
#define DRM_NEBULAE_BO_TYPE_SHIFT		24
#define DRM_NEBULAE_BO_TYPE_MASK		(0xffU << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_GENERIC		(0U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_COMMAND		(1U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_KERNEL		(2U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_KERNARG		(3U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_TYPE_SCRATCH		(4U << DRM_NEBULAE_BO_TYPE_SHIFT)
#define DRM_NEBULAE_BO_FLAGS			(DRM_NEBULAE_BO_WC | \
						 DRM_NEBULAE_BO_NO_AUTO_BIND | \
						 DRM_NEBULAE_BO_PLACEMENT_MASK | \
						 DRM_NEBULAE_BO_TYPE_MASK)

#define DRM_NEBULAE_SUBMIT_NDRANGE		1

#define DRM_NEBULAE_SUBMIT_ASYNC		(1U << 0)
#define DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ	(1U << 1)
#define DRM_NEBULAE_SUBMIT_FLAGS		(DRM_NEBULAE_SUBMIT_ASYNC | \
						 DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ)

#define DRM_NEBULAE_HWCTX_PRIORITY_LOW		(-1)
#define DRM_NEBULAE_HWCTX_PRIORITY_NORMAL	0
#define DRM_NEBULAE_HWCTX_PRIORITY_HIGH		1
#define DRM_NEBULAE_HWCTX_STATE_ACTIVE		0
#define DRM_NEBULAE_HWCTX_STATE_FAULTED		1

#define DRM_NEBULAE_HWCTX_CONFIG_PRIORITY	0
#define DRM_NEBULAE_HWCTX_CONFIG_QOS		1

#define DRM_NEBULAE_SYNC_TO_DEVICE		(1U << 0)
#define DRM_NEBULAE_SYNC_FROM_DEVICE		(1U << 1)
#define DRM_NEBULAE_SYNC_BIDIRECTIONAL		(DRM_NEBULAE_SYNC_TO_DEVICE | \
						 DRM_NEBULAE_SYNC_FROM_DEVICE)
#define DRM_NEBULAE_SYNC_FLAGS			DRM_NEBULAE_SYNC_BIDIRECTIONAL

#define DRM_NEBULAE_RESET_SOFT			0

#define DRM_NEBULAE_CMD_NDRANGE			1
#define DRM_NEBULAE_CMD_DEPENDENCY		2
#define DRM_NEBULAE_CMD_SIGNAL			3
#define DRM_NEBULAE_CMD_TLB_INVALIDATE		4

#define DRM_NEBULAE_TLB_INVALIDATE_ALL		(1U << 0)
#define DRM_NEBULAE_TLB_INVALIDATE_ASID		(1U << 1)
#define DRM_NEBULAE_TLB_INVALIDATE_RANGE	(1U << 2)
#define DRM_NEBULAE_TLB_INVALIDATE_FLAGS	(DRM_NEBULAE_TLB_INVALIDATE_ALL | \
						 DRM_NEBULAE_TLB_INVALIDATE_ASID | \
						 DRM_NEBULAE_TLB_INVALIDATE_RANGE)

#define DRM_NEBULAE_VM_BIND_SIGNAL_SYNCOBJ	(1U << 0)
#define DRM_NEBULAE_VM_BIND_EX_FLAGS		DRM_NEBULAE_VM_BIND_SIGNAL_SYNCOBJ

#define DRM_NEBULAE_EXEC_ASYNC			DRM_NEBULAE_SUBMIT_ASYNC
#define DRM_NEBULAE_EXEC_SIGNAL_SYNCOBJ		DRM_NEBULAE_SUBMIT_SIGNAL_SYNCOBJ
#define DRM_NEBULAE_EXEC_IN_SYNCOBJ		(1U << 2)
#define DRM_NEBULAE_EXEC_FLAGS			(DRM_NEBULAE_EXEC_ASYNC | \
						 DRM_NEBULAE_EXEC_SIGNAL_SYNCOBJ | \
						 DRM_NEBULAE_EXEC_IN_SYNCOBJ)

#define DRM_NEBULAE_INFO_RESOURCE		0
#define DRM_NEBULAE_INFO_TELEMETRY		1
#define DRM_NEBULAE_INFO_POWER_STATE		2
#define DRM_NEBULAE_INFO_FIRMWARE_VERSION	3
#define DRM_NEBULAE_INFO_HWCTX_STATUS		4
#define DRM_NEBULAE_INFO_FAULT_REPORT		5
#define DRM_NEBULAE_INFO_CP_QUEUE		6

#define DRM_NEBULAE_FAULT_VALID			(1U << 0)

#define DRM_NEBULAE_ARRAY_HWCTX			0
#define DRM_NEBULAE_ARRAY_BO_USAGE		1

#define DRM_NEBULAE_STATE_POWER_MODE		0

#define DRM_NEBULAE_POWER_DEFAULT		0
#define DRM_NEBULAE_POWER_LOW			1
#define DRM_NEBULAE_POWER_BALANCED		2
#define DRM_NEBULAE_POWER_HIGH			3

#define DRM_NEBULAE_HEALTH_OK			0
#define DRM_NEBULAE_HEALTH_FAULTED		1

struct drm_nebulae_param {
	__u32 param;
	__u32 index;
	__u64 value;
};

struct drm_nebulae_bo_create {
	__u64 size;
	__u32 flags;
	__u32 handle;
	__u64 va;
};

struct drm_nebulae_bo_info {
	__u32 handle;
	__u32 flags;
	__u64 size;
	__u64 va;
	__u64 mmap_offset;
	__u64 mapped_size;
	__u64 map_offset;
};

struct drm_nebulae_mmap_bo {
	__u32 handle;
	__u32 flags;
	__u64 offset;
};

struct drm_nebulae_vm_bind {
	__u32 handle;
	__u32 flags;
	__u64 va;
	__u64 size;
	__u64 offset;
};

struct drm_nebulae_vm_unbind {
	__u32 handle;
	__u32 flags;
	__u64 va;
	__u64 size;
};

struct drm_nebulae_vm_bind_ex {
	__u32 handle;
	__u32 flags;
	__u64 va;
	__u64 size;
	__u64 offset;
	__u32 out_syncobj;
	__u32 pad;
	__u64 seq;
};

struct drm_nebulae_vm_unbind_ex {
	__u32 handle;
	__u32 flags;
	__u64 va;
	__u64 size;
	__u32 out_syncobj;
	__u32 pad;
	__u64 seq;
};

struct drm_nebulae_ndrange {
	__u64 code_addr;
	__u64 kernarg_addr;
	__u64 scratch_addr;
	__u32 grid_size[3];
	__u32 workgroup_size[3];
	__u32 sr_count;
	__u32 vr_count;
	__u32 private_segment_size;
	__u32 scalar_work_item_mode;
	__u32 wave_count;
	__u32 wgm_bytes;
	__u64 max_steps;
};

struct drm_nebulae_submit {
	__u32 type;
	__u32 flags;
	__u64 sync_bo_handles;
	__u32 sync_bo_count;
	__u32 out_syncobj;
	__u64 seq;
	union {
		struct drm_nebulae_ndrange ndrange;
		__u64 raw[16];
	};
};

struct drm_nebulae_submit_ex {
	__u32 type;
	__u32 flags;
	__u32 hwctx_id;
	__u32 out_syncobj;
	__u64 sync_bo_handles;
	__u32 sync_bo_count;
	__u32 pad;
	__u64 seq;
	union {
		struct drm_nebulae_ndrange ndrange;
		__u64 raw[16];
	};
};

struct drm_nebulae_submit_cmd_bo {
	__u32 hwctx_id;
	__u32 flags;
	__u32 handle;
	__u32 offset;
	__u32 size;
	__u32 cmd_count;
	__u32 in_syncobj;
	__u32 out_syncobj;
	__s32 in_fence_fd;
	__s32 out_fence_fd;
	__u64 seq;
	__u64 cookie;
	__u64 message_id;
	__u64 timeout_ns;
	__u32 status;
	__s32 driver_error;
};

struct drm_nebulae_job_wait {
	__u64 seq;
	__u64 timeout_ns;
	__u32 status;
	__u32 pad;
};

struct drm_nebulae_vm_info {
	__u32 flags;
	__u32 pad;
	__u64 va_start;
	__u64 va_size;
	__u64 page_size;
	__u64 pt_base;
	__u64 pt_size;
	__u64 mapped_bytes;
	__u32 bind_count;
	__u32 asid;
};

struct drm_nebulae_bo_sync {
	__u32 handle;
	__u32 flags;
	__u64 offset;
	__u64 size;
};

struct drm_nebulae_reset {
	__u32 flags;
	__u32 status;
};

struct drm_nebulae_hwctx_create {
	__u32 flags;
	__s32 priority;
	__u32 hwctx_id;
	__u32 pad;
};

struct drm_nebulae_hwctx_destroy {
	__u32 hwctx_id;
	__u32 flags;
};

struct drm_nebulae_hwctx_info {
	__u32 hwctx_id;
	__u32 flags;
	__s32 priority;
	__u32 state;
	__u64 submitted_jobs;
	__u64 completed_jobs;
	__u64 faulted_jobs;
};

struct drm_nebulae_qos_info {
	__u32 gops;
	__u32 fps;
	__u32 dma_bandwidth;
	__u32 latency_us;
	__u32 frame_exec_time_us;
	__s32 priority;
	__u32 flags;
	__u32 pad;
};

struct drm_nebulae_hwctx_config {
	__u32 hwctx_id;
	__u32 param;
	__u64 value;
	__u32 size;
	__u32 flags;
};

struct drm_nebulae_cmd_header {
	__u32 type;
	__u32 size;
};

struct drm_nebulae_cmd_ndrange {
	struct drm_nebulae_cmd_header header;
	__u32 flags;
	__u32 pad;
	__u64 sync_bo_handles;
	__u32 sync_bo_count;
	__u32 pad2;
	struct drm_nebulae_ndrange ndrange;
};

struct drm_nebulae_cmd_sync {
	struct drm_nebulae_cmd_header header;
	__u32 handle;
	__u32 flags;
	__u64 point;
};

struct drm_nebulae_cmd_tlb_invalidate {
	struct drm_nebulae_cmd_header header;
	__u32 flags;
	__u32 asid;
	__u64 va;
	__u64 size;
};

struct drm_nebulae_exec_cmd {
	__u32 hwctx_id;
	__u32 flags;
	__u64 cmds;
	__u32 cmd_count;
	__u32 in_syncobj;
	__u32 out_syncobj;
	__s32 in_fence_fd;
	__s32 out_fence_fd;
	__u32 pad;
	__u64 seq;
};

struct drm_nebulae_get_info {
	__u32 param;
	__u32 index;
	__u32 buffer_size;
	__u32 flags;
	__u64 buffer;
};

struct drm_nebulae_set_state {
	__u32 param;
	__u32 flags;
	__u32 buffer_size;
	__u32 pad;
	__u64 buffer;
};

struct drm_nebulae_get_array {
	__u32 param;
	__u32 element_size;
	__u32 element_count;
	__u32 pad;
	__u64 buffer;
};

struct drm_nebulae_resource_info {
	__u64 caps;
	__u64 vram_size;
	__u64 vm_start;
	__u64 vm_size;
	__u64 page_size;
	__u64 pt_base;
	__u64 pt_size;
	__u64 cmd_base;
	__u32 max_hwctx;
	__u32 max_sync_bo_count;
	__u32 scheduler_engine_count;
	__u32 doorbell_count;
};

struct drm_nebulae_telemetry_info {
	__u64 submitted_jobs;
	__u64 completed_jobs;
	__u64 faulted_jobs;
	__u64 job_seq;
	__u64 completed_seq;
	__u64 hw_job_seq;
	__u64 hw_completed_seq;
	__u32 last_error;
	__u32 power_mode;
};

struct drm_nebulae_power_state {
	__u32 mode;
	__u32 flags;
};

struct drm_nebulae_firmware_version {
	__u32 simx_version;
	__u32 simx_caps;
	__u32 driver_major;
	__u32 driver_minor;
};

struct drm_nebulae_hwctx_status {
	__u32 hwctx_id;
	__u32 context_id;
	__s32 priority;
	__u32 state;
	__u64 submitted_jobs;
	__u64 completed_jobs;
	__u64 faulted_jobs;
	__u32 health;
	__u32 last_error;
	struct drm_nebulae_qos_info qos;
};

struct drm_nebulae_fault_report {
	__u64 seq;
	__u64 fault_va;
	__u64 pt_base;
	__u64 faulted_jobs;
	__u32 hwctx_id;
	__u32 context_id;
	__u32 asid;
	__u32 status;
	__s32 driver_error;
	__u32 flags;
};

struct drm_nebulae_cp_queue_info {
	__u64 ring_base;
	__u64 ring_size;
	__u64 submitted_cookie;
	__u64 completed_cookie;
	__u32 queue_id;
	__u32 doorbell_id;
	__u32 flags;
	__u32 pad;
};

struct drm_nebulae_hwctx_entry {
	__u32 hwctx_id;
	__u32 context_id;
	__s32 priority;
	__u32 state;
	__u64 submitted_jobs;
	__u64 completed_jobs;
	__u64 faulted_jobs;
	__u32 health;
	__u32 pad;
};

struct drm_nebulae_bo_usage {
	__u64 total_bytes;
	__u64 bound_bytes;
	__u32 bo_count;
	__u32 bound_count;
};

#if defined(__cplusplus)
}
#endif

#endif /* __UAPI_NEBULAE_ACCEL_H__ */
