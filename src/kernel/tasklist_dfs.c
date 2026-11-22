// SPDX-License-Identifier: GPL-2.0
/*
 * tasklist_dfs.c
 * Iterative DFS using a best-effort process snapshot.
 */

#define pr_fmt(fmt) "tasklist_dfs: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/rcupdate.h>
#include <linux/string.h>

#include "common.h"

#define MAX_TASKS 16384
#define MAX_INDENT 32

struct dfs_record {
	int pid;
	int ppid;
	int depth;
	int parent_index;
	char state;
	char comm[TASK_COMM_LEN];
};

struct dfs_frame {
	int index;
	int depth;
};

static struct dfs_record *records;
static struct dfs_frame *stack;
static int task_count;

static int find_task(int pid)
{
	int i;

	for (i = 0; i < task_count; i++) {
		if (records[i].pid == pid)
			return i;
	}

	return -1;
}

static void capture_tasks(void)
{
	struct task_struct *task;

	task_count = 0;

/* Include init_task (PID 0) as the root of the process tree. */
records[task_count].pid = 0;
records[task_count].ppid = -1;
records[task_count].depth = -1;
records[task_count].parent_index = -1;
records[task_count].state = task_state_to_char(&init_task);

strscpy(records[task_count].comm,
        "swapper/0",
        TASK_COMM_LEN);

task_count++;

rcu_read_lock();

	for_each_process(task) {
		struct dfs_record *r;

		if (task_count >= MAX_TASKS)
			break;

		r = &records[task_count++];

		r->pid = task_pid_nr(task);
		r->ppid = task_ppid_safe(task);
		r->state = task_state_to_char(task);
		get_task_comm(r->comm, task);
		r->depth = -1;
		r->parent_index = -1;
	}

	rcu_read_unlock();
}

static void build_tree(void)
{
	int i;

	for (i = 0; i < task_count; i++)
		records[i].parent_index =
			find_task(records[i].ppid);
}

static void iterative_dfs(void)
{
	int sp = 0;
	int visited = 0;
	int max_depth = 0;
	int root, j;

	/*
	 * PID 0-ээс эхэлж, дараа нь snapshot-д
	 * эцэг нь байхгүй процессуудыг боловсруулна.
	 */
	for (root = 0; root < task_count; root++) {
		int root_index;

		if (root == 0) {
			root_index = find_task(0);
		} else {
			root_index = root;
		}

		if (root_index < 0)
			continue;

		if (records[root_index].depth != -1)
			continue;

		if (root != 0 &&
		    records[root_index].parent_index != -1)
			continue;

		sp = 0;

		stack[sp].index = root_index;
		stack[sp].depth = 0;
		sp++;

		while (sp > 0) {
			struct dfs_frame frame = stack[--sp];
			struct dfs_record *r = &records[frame.index];
			int indent = min(frame.depth, MAX_INDENT);

			if (r->depth != -1)
				continue;

			r->depth = frame.depth;
			visited++;

			if (frame.depth > max_depth)
				max_depth = frame.depth;

			pr_info(
				"depth=%d %*s%s [pid=%d ppid=%d state=%c]\n",
				frame.depth,
				indent * 2, "",
				r->comm,
				r->pid,
				r->ppid,
				r->state
			);

			for (j = task_count - 1; j >= 0; j--) {
				if (records[j].parent_index != frame.index)
					continue;

				if (records[j].depth != -1)
					continue;

				if (sp >= MAX_TASKS) {
					pr_warn(
						"DFS stack capacity reached\n"
					);
					break;
				}

				stack[sp].index = j;
				stack[sp].depth = frame.depth + 1;
				sp++;
			}
		}
	}

	pr_info(
		"DFS visited %d/%d processes, max depth=%d\n",
		visited, task_count, max_depth
	);

	if (visited != task_count)
		pr_warn(
			"Some processes were not visited: %d\n",
			task_count - visited
		);
}

static int __init tasklist_dfs_init(void)
{
	records = kvcalloc(MAX_TASKS, sizeof(*records), GFP_KERNEL);
	if (!records)
		return -ENOMEM;

	stack = kvcalloc(MAX_TASKS, sizeof(*stack), GFP_KERNEL);
	if (!stack) {
		kvfree(records);
		return -ENOMEM;
	}

	pr_info("Loading iterative DFS module\n");

	capture_tasks();
	build_tree();
	iterative_dfs();

	kvfree(stack);
	kvfree(records);

	stack = NULL;
	records = NULL;

	return 0;
}

static void __exit tasklist_dfs_exit(void)
{
	pr_info("Removing iterative DFS module\n");
}

module_init(tasklist_dfs_init);
module_exit(tasklist_dfs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BD1 team");
MODULE_DESCRIPTION("Iterative DFS process tree with best-effort snapshot");
