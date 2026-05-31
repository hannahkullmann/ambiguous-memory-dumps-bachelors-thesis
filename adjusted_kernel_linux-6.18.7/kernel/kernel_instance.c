// kernel/kernel_instance.c
#include <linux/kernel_instance.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/seq_file.h>
#include <linux/rwlock.h>
#include <linux/sched/task.h>
#include <linux/sched/signal.h>
#include <linux/list.h>


#include <linux/fs.h>
#include <linux/mman.h>
#include <linux/shmem_fs.h>


int ki_mmap_check_ghost(struct file *file, unsigned long map_flags)
{
	if (!file) // Allow mapping if no file is provided (anonymous mapping)
		return 0;
	if ((map_flags & MAP_TYPE) != MAP_SHARED) // Allow mapping if the mapping is not shared (e.g. MAP_PRIVATE)
		return 0;
	if (shmem_file(file)) // Allow mapping if the file is a shared memory file
		return 0;
	return -EACCES; // Permission denied
}


void ki_move_task(struct task_struct *task, struct kernel_instance *new_ki)
{
	struct list_head *target;

	// Check if any argument is NULL
	if (WARN_ON_ONCE(!task || !new_ki))
		return;

	// Get the target list head, default: global init_task list
	target = READ_ONCE(new_ki->tasks_head);
	if (!target)
		target = &init_task.tasks;

	// Lock global task list to prevent concurrent modifications
	write_lock_irq(&tasklist_lock);

	// If task is currently on a list, remove it
	if (!list_empty(&task->tasks))
		list_del_rcu(&task->tasks);

	// Set the task's kernel instance pointer
	WRITE_ONCE(task->ki, new_ki);

	// Add the task to the target list
	list_add_tail_rcu(&task->tasks, target);

	// Unlock the task list
	write_unlock_irq(&tasklist_lock);
}


// Userspace interface to migrate tasks between kernel instances
/*
 * @file:  Pointer to the file structure (unused).
 * @buf:   User buffer containing the command string ("PID KI_ID").
 * @count: Length of the input string.
 * @ppos:  File position pointer (unused).
*/
static ssize_t ki_ctrl_write(struct file *file, const char __user *buf,
			     size_t count, loff_t *ppos)
{
	char input[64];
	int target_pid, target_ki_id;
	struct task_struct *task;

	// Safety Check
	if (count >= sizeof(input))
		return -EINVAL;
	if (copy_from_user(input, buf, count)) // Transfer data from User Space to Kernel Space
		return -EFAULT;
	input[count] = '\0';

	// Check format
	if (sscanf(input, "%d %d", &target_pid, &target_ki_id) != 2) {
		pr_err("[BA Adjustments] Invalid format in ki_ctrl\n");
		return -EINVAL; // Invalid format
	}

	// Check if input is valid
	if (target_ki_id != 0 && target_ki_id != 1) {
		pr_err("[BA Adjustments] ki_ctrl: instance id must be 0 (boot) or 1 (ghost), got %d\n",
		       target_ki_id);
		return -EINVAL; // Invalid format
	}

	rcu_read_lock(); // Lock the task list to prevent concurrent modifications
	task = find_task_by_vpid(target_pid); // Find the task by PID
	if (!task) {
		rcu_read_unlock(); // Unlock the task list
		pr_err("[BA Adjustments] ki_ctrl: pid %d not found (wrong namespace or race?)\n",
		       target_pid);
		return -ESRCH; // PID not found
	}

	// Get the task struct and unlock the task list
	get_task_struct(task);
	rcu_read_unlock();

	// Migrate the task to the new kernel instance
	ki_move_task(task, target_ki_id == 1 ? ghost_kernel_instance : boot_kernel_instance);
	pr_info("[BA Adjustments] Task %d moved to %s instance\n",
		target_pid, target_ki_id == 1 ? "ghost" : "boot");
	put_task_struct(task); // Release the task struct

	return count;
}

// Operations for proc interface
static const struct proc_ops ki_ctrl_ops = {
	.proc_write = ki_ctrl_write,		// Write to file
};

// Userspace interface to show the kernel instance ID of the current task
static int ki_id_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", task_ki(current)->id);
	return 0;
}

// Proc wrapper: Link between single_open and ki_id_show
static int ki_id_open(struct inode *inode, struct file *file)
{
	return single_open(file, ki_id_show, NULL);
}

// Operations for proc interface
static const struct proc_ops ki_id_ops = {
	.proc_open    = ki_id_open, 		// Open file
	.proc_read    = seq_read, 			// Read file
	.proc_lseek   = seq_lseek, 			// Seek file
	.proc_release = single_release, 	// Close file and release memory
};


// Userspace interface to show the physical window of the ghost cage
static int ki_layout_show(struct seq_file *m, void *v)
{
	// Read the start and size of the ghost cage
	phys_addr_t cage_start = READ_ONCE(ghost_ki_mem_start);
	phys_addr_t cage_size  = READ_ONCE(ghost_ki_mem_size);

	// Print the start and size of the ghost cage
	seq_printf(m, "cage_start=%pa\n", &cage_start);
	seq_printf(m, "cage_size=%llu\n", (unsigned long long)cage_size);
	return 0;
}

// Proc wrapper: Link between single_open and ki_layout_show
static int ki_layout_open(struct inode *inode, struct file *file)
{
	return single_open(file, ki_layout_show, NULL);
}

// Operations for proc interface
static const struct proc_ops ki_layout_ops = {
	.proc_open    = ki_layout_open, 		// Open file
	.proc_read    = seq_read, 				// Read file
	.proc_lseek   = seq_lseek, 				// Seek file
	.proc_release = single_release, 		// Close file and release memory
};

void create_ki_proc_interface(void)
{
	struct proc_dir_entry *entry;

	// Create files in /proc

	entry = proc_create("ki_ctrl", 0600, NULL, &ki_ctrl_ops); 	 	// ki migration control
	proc_create("ki_id", 0444, NULL, &ki_id_ops); 					// ki status
	proc_create("ki_layout", 0400, NULL, &ki_layout_ops); 			// memory layout
	if (!entry)
		pr_err("[BA Adjustments] failed to create /proc/ki_ctrl\n");
	else
		pr_debug("[BA Adjustments] /proc/ki_{ctrl,id,layout} ready\n");
}


// Entry point for the kernel instance subsystem
// Deleted after the module is loaded
static int __init ki_memory_protection_init(void)
{
	create_ki_proc_interface();
	return 0;
}

late_initcall(ki_memory_protection_init);
