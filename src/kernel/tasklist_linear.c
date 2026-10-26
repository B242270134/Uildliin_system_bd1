// SPDX-License-Identifier: GPL-2.0
/*
 * tasklist_linear.c - Part I: Iterating over Tasks Linearly
 * Operating System Concepts 10e, Chapter 3, Programming Project 3.
 *
 * for_each_process() макрогоор системийн бүх task-ыг шугаман гүйж,
 * command, state, pid-ийг kernel log buffer руу (dmesg) бичнэ.
 */
#define pr_fmt(fmt) "tasklist_linear: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>	/* for_each_process() */
#include <linux/rcupdate.h>
#include "common.h"

static int __init tasklist_linear_init(void)
{
	struct task_struct *task;
	int count = 0;

	pr_info("Loading module - linear task listing\n");
	pr_info("%-16s %7s %7s %5s %8s\n", "COMMAND", "PID", "PPID", "STATE", "RAW");

	/* Task жагсаалт нь RCU-гаар хамгаалагдсан тул уншихдаа түгжинэ. */
	rcu_read_lock();
	for_each_process(task) {
		pr_info("%-16s %7d %7d %5c %8u\n",
			task->comm,
			task_pid_nr(task),
			task_ppid_safe(task),
			task_state_to_char(task),
			TASK_STATE_RAW(task));
		count++;
	}
	rcu_read_unlock();

	pr_info("Total tasks listed: %d\n", count);
	return 0;
}

static void __exit tasklist_linear_exit(void)
{
	pr_info("Removing module\n");
}

module_init(tasklist_linear_init);
module_exit(tasklist_linear_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OSC Ch.3 Project 3 Part I - list tasks linearly");
MODULE_AUTHOR("BD1 team");
