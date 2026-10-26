// SPDX-License-Identifier: GPL-2.0
/*
 * tasktree.c - UI-д зориулсан өргөтгөл.
 *
 * /proc/tasktree файл үүсгэнэ. Файлыг нээх (open) бүрт kernel тухайн
 * агшны task-уудыг ХОЁР үе шаттай авна:
 *
 *   1. Capture (rcu_read_lock дотор, унтахгүй, санах ой хуваарилахгүй):
 *      - LINEAR: for_each_process() дарааллаар PID-үүд (Part I)
 *      - DFS:    init_task-аас ТОДОРХОЙ СТЕКЭЭР (рекурсгүй) children/sibling
 *                гүйлт (Part II). Процесс бүрийн БҮХ thread-ийн children
 *                жагсаалтыг гүйнэ: thread fork() хийсэн хүүхэд тэр thread-ийн
 *                жагсаалтад ордог.
 *      Task бүрийн талбаруудыг урьдчилан хуваарилсан массивт хуулна.
 *   2. Show: хуулсан өгөгдлөөс JSON хэвлэнэ (lock-гүй).
 *
 * Иймээс `cat /proc/tasktree`-г давтан ажиллуулах бүрт шинэ snapshot гарна.
 *
 * Module параметр (insmod үед эсвэл /sys/module/tasktree/parameters/-аар):
 *   show_threads=0|1    thread-уудыг процессынхоо хүүхэд болгон нэмэх
 *   max_tasks=16..16384 snapshot-ын дээд хэмжээ (анхны утга 4096).
 *                       Хүрвэл JSON-д "truncated":true гэж тэмдэглэнэ.
 */
#define pr_fmt(fmt) "tasktree: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>	/* for_each_process, for_each_thread */
#include <linux/sched/task.h>	/* init_task */
#include <linux/cred.h>
#include <linux/uidgid.h>
#include <linux/list.h>
#include <linux/rcupdate.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/mm.h>		/* kvmalloc_array, kvfree */
#include <linux/atomic.h>
#include <linux/timekeeping.h>
#include <linux/utsname.h>
#include "common.h"

#define PROC_NAME "tasktree"
#define MIN_TASKS 16
#define MAX_TASKS 16384

/* ------------------------------------------------------------ parameters */
static bool show_threads;
module_param(show_threads, bool, 0644);
MODULE_PARM_DESC(show_threads, "Also emit non-leader threads as child nodes (default 0)");

static int max_tasks = 4096;
static int max_tasks_set(const char *val, const struct kernel_param *kp)
{
	int n, ret = kstrtoint(val, 10, &n);

	if (ret)
		return ret;
	if (n < MIN_TASKS || n > MAX_TASKS) {
		pr_err("max_tasks=%d invalid (allowed %d..%d)\n", n, MIN_TASKS, MAX_TASKS);
		return -EINVAL;
	}
	*(int *)kp->arg = n;
	return 0;
}
static const struct kernel_param_ops max_tasks_ops = {
	.set = max_tasks_set,
	.get = param_get_int,
};
module_param_cb(max_tasks, &max_tasks_ops, &max_tasks, 0644);
MODULE_PARM_DESC(max_tasks, "Snapshot capacity 16..16384 (default 4096)");

static atomic_t snapshot_seq = ATOMIC_INIT(0);

/* ------------------------------------------------------------ snapshot */
struct tg_rec {
	int pid, tgid, ppid, parent, depth, threads, prio;
	unsigned int state_raw, uid;
	u64 start_ns;
	char state;
	bool kthread, thread;
	char comm[TASK_COMM_LEN];
};

struct tg_frame {		/* DFS-ийн тодорхой стекийн элемент */
	struct task_struct *task;
	int depth, parent;
};

struct tg_snap {
	int seq, cap, count, nlinear, max_depth;
	bool truncated, show_threads;
	s64 time;
	struct tg_rec *recs;
	int *linear;
	struct tg_frame *stack;
};

static void tg_record(struct tg_snap *s, struct task_struct *t, int depth,
		      int parent, bool is_thread)
{
	struct tg_rec *r;

	if (s->count >= s->cap) {
		s->truncated = true;
		return;
	}
	r = &s->recs[s->count++];
	r->pid = task_pid_nr(t);
	r->tgid = task_tgid_nr(t);
	r->ppid = task_ppid_safe(t);	/* ps-тэй ижил: real_parent-ийн TGID */
	r->parent = parent;		/* графын эцэг (DFS-ээр ирсэн зангилаа) */
	r->depth = depth;
	r->state = task_state_to_char(t);
	r->state_raw = TASK_STATE_RAW(t);
	r->threads = get_nr_threads(t);
	r->kthread = !!(t->flags & PF_KTHREAD);
	r->thread = is_thread;
	r->uid = from_kuid_munged(&init_user_ns, task_uid(t));
	r->prio = t->prio;
	r->start_ns = t->start_time;
	get_task_comm(r->comm, t);
	if (depth > s->max_depth)
		s->max_depth = depth;
}

static void tg_push(struct tg_snap *s, int *sp, struct task_struct *child,
		    int depth, int parent)
{
	if (*sp >= s->cap) {
		s->truncated = true;
		return;
	}
	s->stack[*sp].task = child;
	s->stack[*sp].depth = depth;
	s->stack[*sp].parent = parent;
	(*sp)++;
}

static void tg_capture(struct tg_snap *s)
{
	struct task_struct *p, *thr, *child;
	struct list_head *list;
	int sp = 0;

	rcu_read_lock();

	/* LINEAR (Part I): for_each_process-ийн дараалал */
	for_each_process(p) {
		if (s->nlinear < s->cap)
			s->linear[s->nlinear++] = task_pid_nr(p);
		else
			s->truncated = true;
	}

	/* DFS (Part II): pre-order, тодорхой стекээр */
	tg_push(s, &sp, &init_task, 0, -1);
	while (sp > 0) {
		struct tg_frame f = s->stack[--sp];
		int base, i, j, pid;

		if (s->count >= s->cap) {
			s->truncated = true;
			break;
		}
		tg_record(s, f.task, f.depth, f.parent, false);
		pid = task_pid_nr(f.task);

		if (s->show_threads && f.task != &init_task) {
			for_each_thread(f.task, thr) {
				if (thr != f.task)
					tg_record(s, thr, f.depth + 1, pid, true);
			}
		}

		base = sp;
		if (f.task == &init_task) {
			list_for_each(list, &f.task->children) {
				child = list_entry(list, struct task_struct, sibling);
				tg_push(s, &sp, child, f.depth + 1, pid);
			}
		} else {
			for_each_thread(f.task, thr) {
				list_for_each(list, &thr->children) {
					child = list_entry(list, struct task_struct, sibling);
					tg_push(s, &sp, child, f.depth + 1, pid);
				}
			}
		}
		/* Стек LIFO тул хүүхдүүдийг урвуулж, эхний хүүхэд эхэлж гарна */
		for (i = base, j = sp - 1; i < j; i++, j--) {
			struct tg_frame tmp = s->stack[i];

			s->stack[i] = s->stack[j];
			s->stack[j] = tmp;
		}
	}
	if (sp > 0)
		s->truncated = true;

	rcu_read_unlock();
}

static void tg_free(struct tg_snap *s)
{
	if (!s)
		return;
	kvfree(s->recs);
	kvfree(s->linear);
	kvfree(s->stack);
	kfree(s);
}

/* ------------------------------------------------------------ JSON output */
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

	seq_printf(m, "{\"source\":\"kernel:/proc/%s\",\"snapshot\":%d,\"time\":%lld,\"kernel\":",
		   PROC_NAME, s->seq, (long long)s->time);
	seq_json_str(m, utsname()->release);
	seq_printf(m, ",\"show_threads\":%s,\"cap\":%d,\"truncated\":%s,\"tasks\":[\n",
		   s->show_threads ? "true" : "false", s->cap,
		   s->truncated ? "true" : "false");

	for (i = 0; i < s->count; i++) {
		struct tg_rec *r = &s->recs[i];

		seq_printf(m, "%s{\"order\":%d,\"pid\":%d,\"tgid\":%d,\"ppid\":%d,\"parent\":%d,\"depth\":%d,\"comm\":",
			   i ? ",\n" : "", i, r->pid, r->tgid, r->ppid, r->parent, r->depth);
		seq_json_str(m, r->comm);
		seq_printf(m, ",\"state\":\"%c\",\"state_raw\":%u,\"threads\":%d,\"kthread\":%s,\"thread\":%s,\"uid\":%u,\"prio\":%d,\"start_ns\":%llu}",
			   r->state, r->state_raw, r->threads,
			   r->kthread ? "true" : "false",
			   r->thread ? "true" : "false",
			   r->uid, r->prio, (unsigned long long)r->start_ns);
	}

	seq_puts(m, "\n],\"linear\":[");
	for (i = 0; i < s->nlinear; i++)
		seq_printf(m, i ? ",%d" : "%d", s->linear[i]);
	seq_printf(m, "],\"count\":%d,\"linear_count\":%d,\"max_depth\":%d}\n",
		   s->count, s->nlinear, s->max_depth);
	return 0;
}

static int tasktree_open(struct inode *inode, struct file *file)
{
	struct tg_snap *s;
	int ret;

	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->cap = READ_ONCE(max_tasks);
	s->show_threads = READ_ONCE(show_threads);
	s->recs = kvmalloc_array(s->cap, sizeof(*s->recs), GFP_KERNEL);
	s->linear = kvmalloc_array(s->cap, sizeof(*s->linear), GFP_KERNEL);
	s->stack = kvmalloc_array(s->cap, sizeof(*s->stack), GFP_KERNEL);
	if (!s->recs || !s->linear || !s->stack) {
		tg_free(s);
		return -ENOMEM;
	}

	/* Нээх бүрт шинэ snapshot: дугаар нэмэгдэж, task-уудыг дахин авна */
	s->seq = atomic_inc_return(&snapshot_seq);
	s->time = ktime_get_real_seconds();
	tg_capture(s);
	kvfree(s->stack);	/* стек зөвхөн capture-д хэрэгтэй */
	s->stack = NULL;

	ret = single_open(file, tasktree_show, s);
	if (ret)
		tg_free(s);
	return ret;
}

static int tasktree_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;

	tg_free(m->private);
	return single_release(inode, file);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 0)
static const struct proc_ops tasktree_fops = {
	.proc_open = tasktree_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = tasktree_release,
};
#else
static const struct file_operations tasktree_fops = {
	.owner = THIS_MODULE,
	.open = tasktree_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = tasktree_release,
};
#endif

static int __init tasktree_init(void)
{
	if (!proc_create(PROC_NAME, 0444, NULL, &tasktree_fops)) {
		pr_err("cannot create /proc/%s\n", PROC_NAME);
		return -ENOMEM;
	}
	pr_info("/proc/%s created (show_threads=%d, max_tasks=%d)\n",
		PROC_NAME, show_threads, max_tasks);
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
MODULE_DESCRIPTION("Expose DFS task tree as JSON in /proc/tasktree for the task-graph UI");
MODULE_AUTHOR("BD1 team");
