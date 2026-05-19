/* monster2.c
 *
 * Kernel module implementing a virtual monster that:
 *   - Part B: has a state (struct kernel_monster) and evolves via monster_tick()
 *   - Part C: evolves automatically every second using a delayed_work
 *   - Part D: exposes its state in /proc/kernel_monster (read with: cat /proc/kernel_monster)
 */

#include <linux/init.h>
#include <linux/jiffies.h>      /* msecs_to_jiffies() */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/proc_fs.h>      /* proc_create(), proc_remove() */
#include <linux/seq_file.h>     /* seq_printf(), single_open(), single_release() */
#include <linux/string.h>       /* strncpy(), strscpy() */
#include <linux/workqueue.h>    /* delayed_work, INIT_DELAYED_WORK, schedule_delayed_work() */

/* =========================================================================
 * Module metadata
 * ========================================================================= */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("DLH");
MODULE_DESCRIPTION("Autonomous kernel monster with /proc interface");

/* =========================================================================
 * Module parameters (set at insmod time, e.g.: insmod monster2.ko name="Godzilla")
 * ========================================================================= */
static char *name    = "Poupoule"; /* Monster name */
static int   hunger  = 20;         /* Initial hunger  [0, 100] */
static int   energy  = 90;         /* Initial energy  [0, 100] */
static int   tick_ms = 1000;       /* Tick period in milliseconds */

module_param(name,    charp, S_IRUGO);
MODULE_PARM_DESC(name,    "The monster name (string)");

module_param(hunger,  int,   S_IRUGO);
MODULE_PARM_DESC(hunger,  "Initial hunger value, integer in [0, 100]");

module_param(energy,  int,   S_IRUGO);
MODULE_PARM_DESC(energy,  "Initial energy value, integer in [0, 100]");

module_param(tick_ms, int,   S_IRUGO);
MODULE_PARM_DESC(tick_ms, "Tick interval in milliseconds (default: 1000)");

/* =========================================================================
 * Part B – Monster state
 * ========================================================================= */

/* The monster's full state, kept in a single structure */
struct kernel_monster {
	char name[32];   /* Monster name */
	int  hunger;     /* How hungry it is  [0, 100] */
	int  energy;     /* How energetic it is [0, 100] */
	int  age;        /* Number of ticks since birth */
	char mood[16];   /* Current mood string */
};

/* The global monster instance (only one monster per module load) */
static struct kernel_monster monster;

/* tick counter – kept separately so the log line can show "Tick N" */
static int tick_count = 0;

/*
 * monster_update_mood() – recomputes the mood string based on current state.
 * Rules:
 *   energy < 20          → "sleepy"
 *   hunger > 80          → "angry"
 *   hunger > 50          → "hungry"
 *   otherwise            → "happy"
 */
static void monster_update_mood(struct kernel_monster *m)
{
	if (m->energy < 20)
		strscpy(m->mood, "sleepy", sizeof(m->mood));
	else if (m->hunger > 80)
		strscpy(m->mood, "angry",  sizeof(m->mood));
	else if (m->hunger > 50)
		strscpy(m->mood, "hungry", sizeof(m->mood));
	else
		strscpy(m->mood, "happy",  sizeof(m->mood));
}

/*
 * monster_tick() – advances the monster state by one time step.
 *
 *  - age    increases by 1 (unbounded)
 *  - hunger increases by 10, clamped to [0, 100]
 *  - energy decreases by 5,  clamped to [0, 100]
 *  - mood   is recomputed
 *  - a status line is printed to the kernel log
 */
static void monster_tick(struct kernel_monster *m)
{
	tick_count++;

	/* Age always grows */
	m->age++;

	/* Hunger increases each tick – clamp to 100 so it never overflows */
	m->hunger += 10;
	if (m->hunger > 100)
		m->hunger = 100;

	/* Energy decreases each tick – clamp to 0 so it never goes negative */
	m->energy -= 5;
	if (m->energy < 0)
		m->energy = 0;

	/* Recompute mood from the new hunger/energy values */
	monster_update_mood(m);

	/* Log the current state so we can follow along in dmesg */
	printk(KERN_INFO "kernel_monster: Tick %d -- name=%s hunger=%d energy=%d mood=%s\n",
	       tick_count, m->name, m->hunger, m->energy, m->mood);
}

/* =========================================================================
 * Part C – Autonomous updates via delayed_work
 *
 * A delayed_work is a kernel mechanism that lets you schedule a function
 * ("handler") to run after a delay, on a shared kernel workqueue thread.
 * After the handler runs, it reschedules itself → periodic behaviour.
 * ========================================================================= */

/* The kernel structure that tracks our scheduled work item */
static struct delayed_work monster_work;

/*
 * monster_work_handler() – called by the kernel workqueue after each delay.
 *
 * @work: pointer to the work_struct embedded inside monster_work.
 *        We don't use it here, but the signature is mandatory.
 */
static void monster_work_handler(struct work_struct *work)
{
	/* Advance the monster state */
	monster_tick(&monster);

	/*
	 * Reschedule ourselves for the next tick.
	 * msecs_to_jiffies() converts milliseconds to the kernel's internal
	 * time unit (jiffies), which depends on CONFIG_HZ.
	 */
	schedule_delayed_work(&monster_work, msecs_to_jiffies(tick_ms));
}

/* =========================================================================
 * Part D – /proc interface
 *
 * The procfs (proc filesystem) lets kernel modules expose information to
 * user-space via virtual files under /proc/.
 * We create /proc/kernel_monster so that "cat /proc/kernel_monster" prints
 * the monster's current state without needing dmesg.
 * ========================================================================= */

/* Handle to the /proc entry – we keep it so we can remove it on exit */
static struct proc_dir_entry *monster_proc_entry;

/*
 * monster_proc_show() – called when user-space reads /proc/kernel_monster.
 *
 * @m: seq_file handle – use seq_printf() instead of printk() here.
 * @v: iterator position (unused for single-page files).
 */
static int monster_proc_show(struct seq_file *m, void *v)
{
	/* seq_printf writes into the seq_file buffer that will be sent to the
	 * user reading the file (e.g. via cat). */
	seq_printf(m, "name:   %s\n", monster.name);
	seq_printf(m, "age:    %d\n", monster.age);
	seq_printf(m, "hunger: %d\n", monster.hunger);
	seq_printf(m, "energy: %d\n", monster.energy);
	seq_printf(m, "mood:   %s\n", monster.mood);

	return 0; /* 0 = success */
}

/*
 * monster_proc_open() – called when user-space opens /proc/kernel_monster.
 *
 * single_open() is a helper for simple files that fit in one page.
 * It ties the file to monster_proc_show().
 */
static int monster_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, monster_proc_show, NULL);
}

/*
 * monster_proc_ops – table of callbacks the kernel calls for each
 * file operation on /proc/kernel_monster.
 * We only need open/read/lseek/release for a read-only file.
 */
static const struct proc_ops monster_proc_ops = {
	.proc_open    = monster_proc_open,  /* called on open()  */
	.proc_read    = seq_read,           /* provided by seq_file, handles read() */
	.proc_lseek   = seq_lseek,          /* provided by seq_file, handles lseek() */
	.proc_release = single_release,     /* provided by seq_file, handles close() */
};

/* =========================================================================
 * Module init / exit
 * ========================================================================= */

/*
 * kernel_monster_init() – called when the module is loaded (insmod).
 *
 * Steps:
 *   1. Validate and clamp module parameters.
 *   2. Initialise the monster struct.
 *   3. Create the /proc entry.
 *   4. Start the periodic work.
 */
static int __init kernel_monster_init(void)
{
	/* --- 1. Validate/clamp parameters ---------------------------------- */
	if (hunger < 0 || hunger > 100) {
		pr_warn("kernel_monster: hunger=%d out of range, clamping to [0,100]\n", hunger);
		hunger = hunger < 0 ? 0 : 100;
	}
	if (energy < 0 || energy > 100) {
		pr_warn("kernel_monster: energy=%d out of range, clamping to [0,100]\n", energy);
		energy = energy < 0 ? 0 : 100;
	}
	if (tick_ms <= 0) {
		pr_warn("kernel_monster: tick_ms=%d invalid, resetting to 1000\n", tick_ms);
		tick_ms = 1000;
	}

	/* --- 2. Initialise monster state ----------------------------------- */
	strscpy(monster.name, name, sizeof(monster.name));
	monster.hunger = hunger;
	monster.energy = energy;
	monster.age    = 0;
	monster_update_mood(&monster); /* compute initial mood */

	pr_info("kernel_monster: %s is born! (hunger=%d, energy=%d, mood=%s)\n",
		monster.name, monster.hunger, monster.energy, monster.mood);

	/* --- 3. Create /proc/kernel_monster -------------------------------- */
	/* S_IRUGO = readable by owner, group, and others; not writable */
	monster_proc_entry = proc_create("kernel_monster", S_IRUGO, NULL, &monster_proc_ops);
	if (!monster_proc_entry) {
		pr_err("kernel_monster: failed to create /proc/kernel_monster\n");
		return -ENOMEM; /* signal out-of-memory error to the kernel */
	}
	pr_info("kernel_monster: /proc/kernel_monster created\n");

	/* --- 4. Start periodic updates ------------------------------------- */
	/* INIT_DELAYED_WORK links monster_work to its handler function */
	INIT_DELAYED_WORK(&monster_work, monster_work_handler);
	/* Schedule the first tick after tick_ms milliseconds */
	schedule_delayed_work(&monster_work, msecs_to_jiffies(tick_ms));
	pr_info("kernel_monster: ticking every %d ms\n", tick_ms);

	return 0; /* 0 = success; any non-zero value aborts the load */
}

/*
 * kernel_monster_exit() – called when the module is unloaded (rmmod).
 *
 * IMPORTANT: every resource allocated in init MUST be released here,
 * otherwise the kernel will have dangling pointers/memory after unload.
 */
static void __exit kernel_monster_exit(void)
{
	/*
	 * cancel_delayed_work_sync() cancels the pending work item and
	 * waits for any currently-running handler to finish before returning.
	 * This prevents the handler from running after the module is gone.
	 */
	cancel_delayed_work_sync(&monster_work);
	pr_info("kernel_monster: periodic work stopped\n");

	/* Remove the /proc entry so cat /proc/kernel_monster no longer works */
	if (monster_proc_entry)
		proc_remove(monster_proc_entry);
	pr_info("kernel_monster: /proc/kernel_monster removed\n");

	pr_info("kernel_monster: %s has died (age=%d)...\n", monster.name, monster.age);
}

/* Register the init and exit functions with the kernel */
module_init(kernel_monster_init);
module_exit(kernel_monster_exit);
