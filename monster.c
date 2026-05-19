/* monster.c */
#include <linux/module.h>
#include <linux/init.h>

/* Meta information */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("DLH");
MODULE_DESCRIPTION("Monster birth and death kernel module");

/* Module parameters */
static char *name = "Unnamed";
static int hunger = 50;
static int energy = 50;

module_param(name, charp, S_IRUGO);
MODULE_PARM_DESC(name, "The monster name (string)");

module_param(hunger, int, S_IRUGO);
MODULE_PARM_DESC(hunger, "Initial hunger value, integer in [0, 100]");

module_param(energy, int, S_IRUGO);
MODULE_PARM_DESC(energy, "Initial energy value, integer in [0, 100]");

static int __init ModuleInit(void) {
	/* Clamp hunger and energy to [0, 100] */
	if (hunger < 0 || hunger > 100) {
		printk(KERN_WARNING "monster: hunger value %d out of range, clamping to [0, 100]\n", hunger);
		hunger = hunger < 0 ? 0 : 100;
	}
	if (energy < 0 || energy > 100) {
		printk(KERN_WARNING "monster: energy value %d out of range, clamping to [0, 100]\n", energy);
		energy = energy < 0 ? 0 : 100;
	}

	printk(KERN_NOTICE "monster: %s is born! (hunger=%d, energy=%d)\n", name, hunger, energy);
	return 0;
}

static void __exit ModuleExit(void) {
	printk(KERN_NOTICE "monster: %s has died...\n", name);
}

/* Define entry point */
module_init(ModuleInit);
/* Define exit point */
module_exit(ModuleExit);
