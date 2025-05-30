// SPDX-License-Identifier: GPL-2.0
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/export.h>
#include <linux/memblock.h>
#include <linux/numa.h>

/* These are not inline because of header tangles. */
#ifdef CONFIG_CPUMASK_OFFSTACK
/**
 * alloc_cpumask_var_node - allocate a struct cpumask on a given node
 * @mask: pointer to cpumask_var_t where the cpumask is returned
 * @flags: GFP_ flags
 * @node: memory node from which to allocate or %NUMA_NO_NODE
 *
 * Only defined when CONFIG_CPUMASK_OFFSTACK=y, otherwise is
 * a nop returning a constant 1 (in <linux/cpumask.h>).
 *
 * Return: TRUE if memory allocation succeeded, FALSE otherwise.
 *
 * In addition, mask will be NULL if this fails.  Note that gcc is
 * usually smart enough to know that mask can never be NULL if
 * CONFIG_CPUMASK_OFFSTACK=n, so does code elimination in that case
 * too.
 */
bool alloc_cpumask_var_node(cpumask_var_t *mask, gfp_t flags, int node)
{
	*mask = kmalloc_node(cpumask_size(), flags, node);

#ifdef CONFIG_DEBUG_PER_CPU_MAPS
	if (!*mask) {
		printk(KERN_ERR "=> alloc_cpumask_var: failed!\n");
		dump_stack();
	}
#endif

	return *mask != NULL;
}
EXPORT_SYMBOL(alloc_cpumask_var_node);

/**
 * alloc_bootmem_cpumask_var - allocate a struct cpumask from the bootmem arena.
 * @mask: pointer to cpumask_var_t where the cpumask is returned
 *
 * Only defined when CONFIG_CPUMASK_OFFSTACK=y, otherwise is
 * a nop (in <linux/cpumask.h>).
 * Either returns an allocated (zero-filled) cpumask, or causes the
 * system to panic.
 */
void __init alloc_bootmem_cpumask_var(cpumask_var_t *mask)
{
	*mask = memblock_alloc_or_panic(cpumask_size(), SMP_CACHE_BYTES);
}

/**
 * free_cpumask_var - frees memory allocated for a struct cpumask.
 * @mask: cpumask to free
 *
 * This is safe on a NULL mask.
 */
void free_cpumask_var(cpumask_var_t mask)
{
	kfree(mask);
}
EXPORT_SYMBOL(free_cpumask_var);

/**
 * free_bootmem_cpumask_var - frees result of alloc_bootmem_cpumask_var
 * @mask: cpumask to free
 */
void __init free_bootmem_cpumask_var(cpumask_var_t mask)
{
	memblock_free(mask, cpumask_size());
}
#endif

/**
 * cpumask_local_spread - select the i'th cpu based on NUMA distances
 * @i: index number
 * @node: local numa_node
 *
 * Return: online CPU according to a numa aware policy; local cpus are returned
 * first, followed by non-local ones, then it wraps around.
 *
 * For those who wants to enumerate all CPUs based on their NUMA distances,
 * i.e. call this function in a loop, like:
 *
 * for (i = 0; i < num_online_cpus(); i++) {
 *	cpu = cpumask_local_spread(i, node);
 *	do_something(cpu);
 * }
 *
 * There's a better alternative based on for_each()-like iterators:
 *
 *	for_each_numa_hop_mask(mask, node) {
 *		for_each_cpu_andnot(cpu, mask, prev)
 *			do_something(cpu);
 *		prev = mask;
 *	}
 *
 * It's simpler and more verbose than above. Complexity of iterator-based
 * enumeration is O(sched_domains_numa_levels * nr_cpu_ids), while
 * cpumask_local_spread() when called for each cpu is
 * O(sched_domains_numa_levels * nr_cpu_ids * log(nr_cpu_ids)).
 */
unsigned int cpumask_local_spread(unsigned int i, int node)
{
	unsigned int cpu;

	/* Wrap: we always want a cpu. */
	i %= num_online_cpus();

	cpu = sched_numa_find_nth_cpu(cpu_online_mask, i, node);

	WARN_ON(cpu >= nr_cpu_ids);
	return cpu;
}
EXPORT_SYMBOL(cpumask_local_spread);

static DEFINE_PER_CPU(int, distribute_cpu_mask_prev);

/**
 * cpumask_any_and_distribute - Return an arbitrary cpu within src1p & src2p.
 * @src1p: first &cpumask for intersection
 * @src2p: second &cpumask for intersection
 *
 * Iterated calls using the same srcp1 and srcp2 will be distributed within
 * their intersection.
 *
 * Return: >= nr_cpu_ids if the intersection is empty.
 */
unsigned int cpumask_any_and_distribute(const struct cpumask *src1p,
			       const struct cpumask *src2p)
{
	unsigned int next, prev;

	/* NOTE: our first selection will skip 0. */
	prev = __this_cpu_read(distribute_cpu_mask_prev);

	next = cpumask_next_and_wrap(prev, src1p, src2p);
	if (next < nr_cpu_ids)
		__this_cpu_write(distribute_cpu_mask_prev, next);

	return next;
}
EXPORT_SYMBOL(cpumask_any_and_distribute);

/**
 * cpumask_any_distribute - Return an arbitrary cpu from srcp
 * @srcp: &cpumask for selection
 *
 * Return: >= nr_cpu_ids if the intersection is empty.
 */
unsigned int cpumask_any_distribute(const struct cpumask *srcp)
{
	unsigned int next, prev;

	/* NOTE: our first selection will skip 0. */
	prev = __this_cpu_read(distribute_cpu_mask_prev);
	next = cpumask_next_wrap(prev, srcp);
	if (next < nr_cpu_ids)
		__this_cpu_write(distribute_cpu_mask_prev, next);

	return next;
}
EXPORT_SYMBOL(cpumask_any_distribute);
