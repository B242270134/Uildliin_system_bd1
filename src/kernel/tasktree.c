// SPDX-License-Identifier: GPL-2.0
/*
 * tasktree.c
 *
 * Process snapshot + iterative DFS for /proc/tasktree.
 *
 * The snapshot is best-effort, not atomic.
 * show_threads=1 is temporarily unsupported.
 */

#define pr_fmt(fmt) "tasktree: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/rcupdate.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/atomic.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include <linux/string.h>
#include <linux/errno.h>

#include "common.h"

#define PROC_NAME "tasktree"
#define MIN_TASKS 16
#define MAX_TASKS 16384

/* -------------------------------------------------- parameters */

static bool show_threads;

static int show_threads_set(const char *val,
			    const struct kernel_param *kp)
{
	bool enabled;
	int ret = kstrtobool(val, &enabled);

	if (ret)
		return ret;

	if (enabled)
		return -EOPNOTSUPP;

	*(bool *)kp->arg = false;
	return 0;
}

static const struct kernel_param_ops show_threads_ops = {
	.set = show_threads_set,
	.get = param_get_bool,
};

module_param_cb(show_threads, &show_threads_ops,
		&show_threads, 0644);

MODULE_PARM_DESC(show_threads,
		 "Thread display temporarily unsupported");

static int max_tasks = 4096;

static int max_tasks_set(const char *val,
			 const struct kernel_param *kp)
{
	int n, ret;

	ret = kstrtoint(val, 10, &n);
	if (ret)
		return ret;

	if (n < MIN_TASKS || n > MAX_TASKS)
		return -EINVAL;

	*(int *)kp->arg = n;
	return 0;
}

static const struct kernel_param_ops max_tasks_ops = {
	.set = max_tasks_set,
	.get = param_get_int,
};

module_param_cb(max_tasks, &max_tasks_ops, &max_tasks, 0644);

MODULE_PARM_DESC(max_tasks, "Snapshot capacity 16..16384");

static atomic_t snapshot_seq = ATOMIC_INIT(0);

/* -------------------------------------------------- structures */

struct tg_rec {
	int pid, tgid, ppid, parent, depth, threads, prio;
	unsigned int state_raw, uid;
	u64 start_ns;
	char state;
	bool kthread, thread;
	char comm[TASK_COMM_LEN];
};

struct tg_frame {
	int index;
};

struct tg_snap {
	int *dfs_order;
	int *parent_index;
	int dfs_count;
	int seq;
	int cap;
	int count;
	int nlinear;
	int max_depth;

	bool truncated;
	bool show_threads;

	s64 time;

	struct tg_rec *recs;
	int *linear;
	struct tg_frame *stack;

	/* Snapshot-only tree relationships */
	int *first_child;
	int *next_sibling;
};

/* -------------------------------------------------- record */

static void tg_record(struct tg_snap *s,
		      struct task_struct *t)
{
	struct tg_rec *r;

	if (s->count >= s->cap) {
		s->truncated = true;
		return;
	}

	r = &s->recs[s->count++];

	r->pid = task_pid_nr(t);
	r->tgid = task_tgid_nr(t);
	r->ppid = task_ppid_safe(t);

	r->parent = -1;
	r->depth = -1;

	r->state = task_state_to_char(t);
	r->state_raw = TASK_STATE_RAW(t);

	r->threads = get_nr_threads(t);
	r->kthread = !!(READ_ONCE(t->flags) & PF_KTHREAD);
	r->thread = false;

	r->uid = from_kuid_munged(&init_user_ns, task_uid(t));
	r->prio = READ_ONCE(t->prio);
	r->start_ns = READ_ONCE(t->start_time);

	get_task_comm(r->comm, t);
}

/* -------------------------------------------------- capture */

static void tg_capture(struct tg_snap *s)
{
	struct task_struct *p;

	/*
	 * PID 0 is not returned by for_each_process().
	 */
	rcu_read_lock();

	tg_record(s, &init_task);

	for_each_process(p) {
		if (s->nlinear < s->cap)
			s->linear[s->nlinear++] = task_pid_nr(p);
		else
			s->truncated = true;

		tg_record(s, p);
	}

	rcu_read_unlock();
}

/* -------------------------------------------------- parent lookup */

static int tg_find_pid(struct tg_snap *s, int pid)
{
	int i;

	for (i = 0; i < s->count; i++) {
		if (s->recs[i].pid == pid)
			return i;
	}

	return -1;
}

/* -------------------------------------------------- build tree */

static void tg_build_tree(struct tg_snap *s)
{
	int i;

	/* Бүх холбоосыг эхлүүлэх */
	for (i = 0; i < s->count; i++) {
		s->first_child[i] = -1;
		s->next_sibling[i] = -1;
		s->parent_index[i] = -1;
		s->recs[i].parent = -1;
		s->recs[i].depth = -1;
	}

	if (!s->count)
		return;

	/* PID 0 бол үндэс */
	s->parent_index[0] = -1;

	/* PID/PPID ашиглан эцгийн индексийг тодорхойлох */
	for (i = 1; i < s->count; i++) {
		int parent = tg_find_pid(s, s->recs[i].ppid);

		if (parent < 0 || parent == i) {
			parent = 0;
			s->truncated = true;
		}

		s->parent_index[i] = parent;
	}

	/*
	 * Мөчлөг илрүүлэх.
	 *
	 * Эцгийн холбоосыг дагаж явахад count-оос
	 * олон алхам шаардвал мөчлөг байна.
	 *
	 * Тухайн зангилааг PID 0-д холбоно.
	 */
	for (i = 1; i < s->count; i++) {
    int node_idx = i;
    int steps = 0;

    while (node_idx > 0 && steps < s->count) {
        node_idx = s->parent_index[node_idx];
        steps++;
    }

    if (node_idx != 0) {
        s->parent_index[i] = 0;
        s->truncated = true;

        pr_warn("Invalid parent chain: PID=%d\n",
                s->recs[i].pid);
    }
}
	/*
 * Хүүхдүүдийг буурах индексийн дарааллаар
 * жагсаалтын эхэнд нэмнэ.
 *
 * Ингэснээр linked list өсөх дараалалтай,
 * харин DFS стекээр боловсруулахад урвуу
 * дарааллаар очихыг анхаарна.
 */
for (i = 1; i < s->count; i++) {
	int parent = s->parent_index[i];

	s->next_sibling[i] = s->first_child[parent];
	s->first_child[parent] = i;
}
}

static void tg_dfs(struct tg_snap *s)
{
	int sp = 0;
	int i;

	if (!s->count)
		return;

	s->dfs_count = 0;
	s->max_depth = 0;

	/* PID 0-оос эхлэх */
	s->stack[sp++].index = 0;

	while (sp > 0) {
		struct tg_frame frame = s->stack[--sp];
		struct tg_rec *r = &s->recs[frame.index];
		int parent;
		int child;

		/* Давхар боловсруулахаас хамгаалах */
		if (r->depth >= 0)
			continue;

		parent = s->parent_index[frame.index];

		if (parent < 0) {
			r->depth = 0;
			r->parent = -1;
		} else {
			/*
			 * Эцэг нь DFS-д өмнө нь
			 * боловсруулагдсан байх ёстой.
			 */
			if (s->recs[parent].depth < 0) {
				s->truncated = true;
				continue;
			}

			r->depth = s->recs[parent].depth + 1;
			r->parent = s->recs[parent].pid;
		}

		if (r->depth > s->max_depth)
			s->max_depth = r->depth;

		/* DFS pre-order бүртгэх */
		s->dfs_order[s->dfs_count++] = frame.index;

		/*
		 * Хүүхдүүдийг стек рүү нэмэх.
		 *
		 * tg_build_tree() жагсаалтыг
		 * буурах индексээр байгуулсан.
		 *
		 * Стек LIFO тул DFS нь хүүхдүүдийг
		 * өсөх индексийн дарааллаар боловсруулна.
		 */
		for (child = s->first_child[frame.index];
		     child >= 0;
		     child = s->next_sibling[child]) {

			if (sp >= s->cap) {
				s->truncated = true;
				break;
			}

			s->stack[sp++].index = child;
		}
	}

	/*
	 * Бүх бүртгэгдсэн процесс DFS-д
	 * хамрагдсан эсэхийг шалгах.
	 */
	if (s->dfs_count != s->count) {
		s->truncated = true;

		pr_warn("DFS incomplete: captured=%d visited=%d\n",
			s->count, s->dfs_count);
	}

	for (i = 0; i < s->count; i++) {
		if (s->recs[i].depth < 0) {
			pr_warn("Unreachable PID=%d PPID=%d\n",
				s->recs[i].pid,
				s->recs[i].ppid);
		}
	}
}
static void tg_free(struct tg_snap *s)
{
	if (!s)
		return;

	kvfree(s->dfs_order);
	kvfree(s->parent_index);
	kvfree(s->recs);
	kvfree(s->linear);
	kvfree(s->stack);
	kvfree(s->first_child);
	kvfree(s->next_sibling);

	kfree(s);
}
/* -------------------------------------------------- JSON */

static void seq_json_str(struct seq_file *m, const char *str)
{
	seq_putc(m, '"');

	for (; *str; str++) {
		unsigned char c = (unsigned char)*str;

		if (c == '"' || c == '\\')
			seq_printf(m, "\\%c", c);
		else if (c < 0x20 || c == 0x7f)
			seq_printf(m, "\\u%04x", c);
		else
			seq_putc(m, c);
	}

	seq_putc(m, '"');
}

static int tasktree_show(struct seq_file *m, void *v)
{
	struct tg_snap *s = m->private;
	int i;

	seq_printf(m,
		   "{\"source\":\"kernel:/proc/%s\","
		   "\"snapshot\":%d,\"time\":%lld,\"kernel\":",
		   PROC_NAME, s->seq, (long long)s->time);

	seq_json_str(m, utsname()->release);

	seq_printf(m,
		   ",\"show_threads\":%s,"
		   "\"cap\":%d,\"truncated\":%s,\"tasks\":[\n",
		   s->show_threads ? "true" : "false",
		   s->cap,
		   s->truncated ? "true" : "false");

	for (i = 0; i < s->dfs_count; i++) {
		struct tg_rec *r = &s->recs[s->dfs_order[i]];

		seq_printf(m,
			   "%s{\"order\":%d,\"pid\":%d,"
			   "\"tgid\":%d,\"ppid\":%d,"
			   "\"parent\":%d,\"depth\":%d,\"comm\":",
			   i ? ",\n" : "",
			   i, r->pid, r->tgid,
			   r->ppid, r->parent, r->depth);

		seq_json_str(m, r->comm);

		seq_printf(m,
			   ",\"state\":\"%c\","
			   "\"state_raw\":%u,\"threads\":%d,"
			   "\"kthread\":%s,\"thread\":%s,"
			   "\"uid\":%u,\"prio\":%d,"
			   "\"start_ns\":%llu}",
			   r->state,
			   r->state_raw,
			   r->threads,
			   r->kthread ? "true" : "false",
			   r->thread ? "true" : "false",
			   r->uid,
			   r->prio,
			   (unsigned long long)r->start_ns);
	}

	seq_puts(m, "\n],\"linear\":[");

	for (i = 0; i < s->nlinear; i++)
		seq_printf(m, i ? ",%d" : "%d", s->linear[i]);

	seq_printf(m,
	   "],\"count\":%d,"
	   "\"linear_count\":%d,\"max_depth\":%d}\n",
	   s->dfs_count, s->nlinear, s->max_depth);

	return 0;
}

/* -------------------------------------------------- open */

static int tasktree_open(struct inode *inode, struct file *file)
{
	struct tg_snap *s;
	int ret;

	/* 1. Snapshot бүтцэд санах ой хуваарилах */
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	/* 2. Тохиргооны утгууд */
	s->cap = READ_ONCE(max_tasks);
	s->show_threads = false;

	/* 3. Snapshot-ийн массивууд */
	s->recs = kvcalloc(s->cap,
			   sizeof(*s->recs),
			   GFP_KERNEL);

	s->linear = kvcalloc(s->cap,
			     sizeof(*s->linear),
			     GFP_KERNEL);

	/* 4. Iterative DFS-ийн стек */
	s->stack = kvcalloc(s->cap,
			    sizeof(*s->stack),
			    GFP_KERNEL);

	/* 5. Эцэг-хүүхдийн холбоос */
	s->first_child = kvcalloc(s->cap,
				  sizeof(*s->first_child),
				  GFP_KERNEL);

	s->next_sibling = kvcalloc(s->cap,
				   sizeof(*s->next_sibling),
				   GFP_KERNEL);

	s->parent_index = kvcalloc(s->cap,
				   sizeof(*s->parent_index),
				   GFP_KERNEL);

	/* 6. DFS-ийн дараалал */
	s->dfs_order = kvcalloc(s->cap,
				sizeof(*s->dfs_order),
				GFP_KERNEL);

	/* 7. Санах ой хуваарилалтыг шалгах */
	if (!s->recs ||
	    !s->linear ||
	    !s->stack ||
	    !s->first_child ||
	    !s->next_sibling ||
	    !s->parent_index ||
	    !s->dfs_order) {

		tg_free(s);
		return -ENOMEM;
	}

	/* 8. Snapshot-ийн дугаар, хугацаа */
	s->seq = atomic_inc_return(&snapshot_seq);
	s->time = ktime_get_real_seconds();

	/* 9. Процессын мэдээлэл цуглуулах */
	tg_capture(s);

	/* 10. Эцэг-хүүхдийн мод байгуулах */
	tg_build_tree(s);

	/* 11. Давталттай DFS ажиллуулах */
	tg_dfs(s);

	/* 12. DFS-д ашигласан түр массивуудыг чөлөөлөх */
	kvfree(s->stack);
	kvfree(s->first_child);
	kvfree(s->next_sibling);
	kvfree(s->parent_index);

	s->stack = NULL;
	s->first_child = NULL;
	s->next_sibling = NULL;
	s->parent_index = NULL;

	/*
	 * dfs_order-ийг энд чөлөөлөхгүй.
	 * Учир нь tasktree_show() JSON хэвлэхдээ ашиглана.
	 */

	/* 13. /proc/tasktree файлыг нээх */
	ret = single_open(file, tasktree_show, s);

	if (ret) {
		tg_free(s);
		return ret;
	}

	return 0;
}

static int tasktree_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	tg_free(m->private);

	return single_release(inode, file);
}

/* -------------------------------------------------- proc operations */

static const struct proc_ops tasktree_fops = {
	.proc_open = tasktree_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = tasktree_release,
};

/* -------------------------------------------------- module */

static int __init tasktree_init(void)
{
	if (!proc_create(PROC_NAME, 0444, NULL, &tasktree_fops))
		return -ENOMEM;

	pr_info("/proc/%s created (max_tasks=%d)\n",
		PROC_NAME, max_tasks);

	return 0;
}

static void __exit tasktree_exit(void)
{
	remove_proc_entry(PROC_NAME, NULL);

	pr_info("/proc/%s removed\n", PROC_NAME);
}

module_init(tasktree_init);
module_exit(tasktree_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("BD1 team");
MODULE_DESCRIPTION("Best-effort process snapshot with iterative DFS");
