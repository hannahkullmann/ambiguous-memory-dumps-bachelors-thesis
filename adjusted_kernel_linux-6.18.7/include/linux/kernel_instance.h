#ifndef _LINUX_KERNEL_INSTANCE_H
#define _LINUX_KERNEL_INSTANCE_H

#include <linux/sched.h>
#include <linux/percpu.h>
#include <linux/types.h>
#include <linux/ioport.h>

struct kernel_instance {
	int id;									// Instance ID
	struct task_struct *current_task;		// Pointer to the current task of the instance
	struct list_head *tasks_head;  			// Instance-dependent task-list anchor, new tasks for the current instance are attached here
};

// supervisor-layer variables
extern struct kernel_instance boot_ki;
extern struct kernel_instance ghost_ki;
extern struct kernel_instance *boot_kernel_instance;
extern struct kernel_instance *ghost_kernel_instance;

// Physical addressing (ghost cage window)
extern phys_addr_t ghost_ki_mem_start;
extern phys_addr_t ghost_ki_mem_size;

/**
 * ki_move_task - Migrates a task to a new kernel instance.
 * @task:   Pointer to the task_struct of the process to be moved.
 * @new_ki: Pointer to the destination kernel_instance.
 *
 * Context: Process context. Acquires and releases the global write_lock_irq()
 * on tasklist_lock. Disables interrupts during execution.
 *
 * Return: Void. Emits a kernel warning (WARN_ON_ONCE) if any argument is NULL.
 */
void ki_move_task(struct task_struct *task, struct kernel_instance *new_ki);

/**
 * ki_alloc_ghost_pages - Allocates pages from the ghost page pool.
 * @gfp:   GFP flags controlling the allocation behavior.
 * @order: Allocation order (number of pages to allocate is 2^order).
 *
 * Return: Pointer to the first struct page of the allocated block on success,
 * or NULL if the pool is uninitialized or out of memory.
 */
struct page *ki_alloc_ghost_pages(gfp_t gfp, unsigned int order);


/**
 * ki_free_ghost_pages - Releases ghost pages back to the ghost page pool.
 * @page:  Pointer to the struct page to be freed.
 * @order: Initial allocation order.
 *
 * Context: Process context. Takes internal locks of the gen_pool implementation.
 *
 * Return: Void. Safely bails out if @page or pool is NULL, or if the physical
 * address range does not belong to the gen_pool.
 */
void ki_free_ghost_pages(struct page *page, unsigned int order);

// True if page PFN lies in the ghost cage range.
bool ki_page_in_ghost_cage(const struct page *page);


/**
 * create_ki_proc_interface - Initializes and registers the /proc interface files.
 *
 * Creates three control and status entries under the /proc filesystem:
 * - /proc/ki_ctrl   (Read/Write, Root-only) for process migration.
 * - /proc/ki_id     (Read-only, World-readable) for retrieving instance IDs.
 * - /proc/ki_layout (Read-only, Root-only) for inspecting memory layouts.
 *
 * Return: Void. Logs an error via pr_err() if the critical 'ki_ctrl' entry 
 * fails to allocate.
 */
void create_ki_proc_interface(void);

/**
 * ki_compute_ghost_cage - Calculates and reserves the early boot ghost memory cage.
 *
 * Context: Early boot phase only (memblock subsystem initialization). 
 * Must be called before the page allocator (buddy system) comes online.
 *
 * Return: Void. Disables the cage (sets start and size to 0) and logs a warning
 * if validation or memblock reservation fails.
 */
void __init ki_compute_ghost_cage(void);

//per-CPU declaration of current kernel instance
DECLARE_PER_CPU(struct kernel_instance *, this_ki);


// Pointer to the active struct kernel_instance for the current CPU
static inline struct kernel_instance *current_ki(void)
{
	struct kernel_instance *ki = this_cpu_read(this_ki);
	return ki ? ki : boot_kernel_instance;
}


/**
 * ki_mmap_check_ghost - Validates memory mapping flags against isolation policies.
 * @file:      Pointer to the file structure being mapped, or NULL for anonymous maps.
 * @map_flags: The mmap flags passed from userspace (e.g., MAP_SHARED, MAP_PRIVATE).
 *
 * Context: Process context. Called during the mmap() syscall evaluation phase.
 *
 * Return: 0 if the mapping is allowed, or -EACCES if the mapping violates
 * subsystem security policies.
 */
struct file;
int ki_mmap_check_ghost(struct file *file, unsigned long map_flags);


// Pointer to the struct kernel_instance associated with the task, default: boot instance	
static inline struct kernel_instance *task_ki(const struct task_struct *task)
{
	struct kernel_instance *k;

	if (!task)
		return boot_kernel_instance;
	k = READ_ONCE(task->ki);
	return k ? k : boot_kernel_instance;
}

/**
 * ki_devmem_phys_range_allowed - Validates physical /dev/mem access based on kernel instance ID.
 * @p:     Starting physical address requested for access.
 * @count: Size of the requested memory region in bytes.
 *
 * Return: true if the access is permitted, else false
 */
static inline bool ki_devmem_phys_range_allowed(phys_addr_t p, size_t count)
{
	phys_addr_t gsize = READ_ONCE(ghost_ki_mem_size);
	struct kernel_instance *ki;
	phys_addr_t gstart, gend, end;

	// Check activity of ghost cage

	if (!gsize || !count)
		return true; 		// Allow access if the ghost cage is not initialized or the count is zero

	
	// Get the current kernel instance, default: boot instance
	ki = READ_ONCE(current->ki);
	if (!ki)
		ki = boot_kernel_instance;

	// Integer overflow protection

	if (p + (phys_addr_t)count < p)
		return false;


	// Calculate end address
	end = p + (phys_addr_t)count;
	gstart = READ_ONCE(ghost_ki_mem_start);
	gend = gstart + gsize;

	// Boot instance: allow access to all physical memory
	if (ki->id == 0)
		return true;
	// Ghost instance: allow access if the physical address is within the ghost cage
	if (ki->id == 1)
		return p >= gstart && end <= gend;
	return true;
}

#endif /* _LINUX_KERNEL_INSTANCE_H */
