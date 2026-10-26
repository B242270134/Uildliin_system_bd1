// SPDX-License-Identifier: GPL-2.0
/*
 * tasklist_dfs.c - Part II: Iterating over Tasks with a Depth-First Search Tree
 * Operating System Concepts 10e, Chapter 3, Programming Project 3.
 *
 * init_task-аас эхлээд children / sibling жагсаалтаар DFS хийж,
 * name, state, pid-ийг гүнээр догол мөр гаргаж kernel log руу бичнэ.
 *
 * Сурах бичгийн загвараас ялгаатай нэг засвар: процессын аль ч thread
 * fork() хийж болох ба тэр хүүхэд нь ТУХАЙН THREAD-ийн children
 * жагсаалтад ордог (leader-ийнх биш). Тиймээс процесс бүрийн бүх
 * thread-ийн children жагсаалтыг гүйнэ (for_each_thread). Үгүй бол
 * олон thread-тэй программын хүүхдүүд модноос алга болдог.
 */
#define pr_fmt(fmt) "tasklist_dfs: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>	/* for_each_thread */
#include <linux/sched/task.h>	/* init_task */
#include <linux/list.h>
#include <linux/rcupdate.h>
#include "common.h"

#define MAX_INDENT 32
#define MAX_DEPTH  256		/* kernel stack-ийг хамгаалах хязгаар */

static int dfs_count, dfs_max_depth, dfs_cut;

static void dfs(struct task_struct *task, int depth)
{
	struct task_struct *child, *thr;
	struct list_head *list;
	int indent = depth > MAX_INDENT ? MAX_INDENT : depth;

	dfs_count++;
	if (depth > dfs_max_depth)
		dfs_max_depth = depth;

	/* %*s: depth*2 зайгаар догол мөр гаргаж модны бүтцийг харуулна */
	pr_info("%*s%s [pid=%d ppid=%d state=%c]\n",
		indent * 2, "",
		task->comm,
		task_pid_nr(task),
		task_ppid_safe(task),
		task_state_to_char(task));

	if (depth >= MAX_DEPTH) {	/* рекурс хэт гүнзгийрэхээс сэргийлнэ */
		dfs_cut++;
		return;
	}

	if (task == &init_task) {
		/* init_task (PID 0): сурах бичгийн загвар яг хэвээрээ */
		list_for_each(list, &task->children) {
			child = list_entry(list, struct task_struct, sibling);
			dfs(child, depth + 1);
		}
		return;
	}

	/* children толгойгоос sibling-ээр гинжлэгдэнэ; thread бүрийнхийг гүйнэ */
	for_each_thread(task, thr) {
		list_for_each(list, &thr->children) {
			child = list_entry(list, struct task_struct, sibling);
			dfs(child, depth + 1);
		}
	}
}

static int __init tasklist_dfs_init(void)
{
	pr_info("Loading module - DFS task tree from init_task\n");

	dfs_count = 0;
	dfs_max_depth = 0;
	dfs_cut = 0;

	rcu_read_lock();
	dfs(&init_task, 0);
	rcu_read_unlock();

	pr_info("DFS visited %d tasks, max depth %d%s\n", dfs_count, dfs_max_depth,
		dfs_cut ? " (depth limit reached!)" : "");
	return 0;
}

static void __exit tasklist_dfs_exit(void)
{
	pr_info("Removing module\n");
}

module_init(tasklist_dfs_init);
module_exit(tasklist_dfs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("OSC Ch.3 Project 3 Part II - list tasks with DFS");
MODULE_AUTHOR("BD1 team");
