/*
 * common.h - Kernel хувилбараас хамаарах жижиг туслах макронууд.
 * BD1: Linux Kernel Module for Listing Tasks (OSC 10e, Ch.3, Project 3)
 */
#ifndef TASKLIST_COMMON_H
#define TASKLIST_COMMON_H

#include <linux/version.h>
#include <linux/sched.h>

/* 5.14-с хойш task_struct->state талбарыг __state гэж нэрлэсэн. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 0)
#define TASK_STATE_RAW(t) ((unsigned int)READ_ONCE((t)->__state))
#else
#define TASK_STATE_RAW(t) ((unsigned int)(t)->state)
#endif

/*
 * Эцэг процессын PID-ийг ps / /proc/<pid>/stat-тэй ижил аргаар буцаана.
 * real_parent нь thread байж болно (thread fork() хийсэн үед), тиймээс
 * thread-ийн TID биш, түүний thread group-ийн дугаар (TGID)-ийг авна.
 * init_task-ийн эцэг нь өөрөө тул -1.
 */
static inline int task_ppid_safe(struct task_struct *t)
{
	struct task_struct *p = rcu_dereference(t->real_parent);

	if (!p || p == t)
		return -1;
	return task_tgid_nr(p);
}

#endif /* TASKLIST_COMMON_H */
