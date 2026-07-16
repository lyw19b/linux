/* SPDX-License-Identifier: GPL-2.0-only */
#if !defined(_NEBULAE_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _NEBULAE_TRACE_H

#include <linux/tracepoint.h>

#undef TRACE_SYSTEM
#define TRACE_SYSTEM nebulae
#define TRACE_INCLUDE_FILE nebulae_trace

TRACE_EVENT(nebulae_job,
	TP_PROTO(const char *phase, u64 ctx, u64 user_seq, u64 hw_seq,
		 u32 asid, int error),
	TP_ARGS(phase, ctx, user_seq, hw_seq, asid, error),
	TP_STRUCT__entry(
		__string(phase, phase)
		__field(u64, ctx)
		__field(u64, user_seq)
		__field(u64, hw_seq)
		__field(u32, asid)
		__field(int, error)
	),
	TP_fast_assign(
		__assign_str(phase);
		__entry->ctx = ctx;
		__entry->user_seq = user_seq;
		__entry->hw_seq = hw_seq;
		__entry->asid = asid;
		__entry->error = error;
	),
	TP_printk("%s ctx=%llu user_seq=%llu hw_seq=%llu asid=%u error=%d",
		  __get_str(phase), __entry->ctx, __entry->user_seq,
		  __entry->hw_seq, __entry->asid, __entry->error)
);

TRACE_EVENT(nebulae_fault,
	TP_PROTO(u64 ctx, u64 job_seq, u32 asid, u32 reason, u32 flags,
		 u64 va, u32 hw_status, int error),
	TP_ARGS(ctx, job_seq, asid, reason, flags, va, hw_status, error),
	TP_STRUCT__entry(
		__field(u64, ctx)
		__field(u64, job_seq)
		__field(u32, asid)
		__field(u32, reason)
		__field(u32, flags)
		__field(u64, va)
		__field(u32, hw_status)
		__field(int, error)
	),
	TP_fast_assign(
		__entry->ctx = ctx;
		__entry->job_seq = job_seq;
		__entry->asid = asid;
		__entry->reason = reason;
		__entry->flags = flags;
		__entry->va = va;
		__entry->hw_status = hw_status;
		__entry->error = error;
	),
	TP_printk("ctx=%llu job=%llu asid=%u reason=%u flags=0x%x va=0x%llx hw_status=%u error=%d",
		  __entry->ctx, __entry->job_seq, __entry->asid,
		  __entry->reason, __entry->flags, __entry->va,
		  __entry->hw_status, __entry->error)
);

TRACE_EVENT(nebulae_reset,
	TP_PROTO(const char *phase, u32 reason, u64 count, int error),
	TP_ARGS(phase, reason, count, error),
	TP_STRUCT__entry(
		__string(phase, phase)
		__field(u32, reason)
		__field(u64, count)
		__field(int, error)
	),
	TP_fast_assign(
		__assign_str(phase);
		__entry->reason = reason;
		__entry->count = count;
		__entry->error = error;
	),
	TP_printk("%s reason=%u count=%llu error=%d", __get_str(phase),
		  __entry->reason, __entry->count, __entry->error)
);

#endif

#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/nebulae
#include <trace/define_trace.h>
