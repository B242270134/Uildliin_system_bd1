/*
 * thread_fork.c - "thread-ээс fork хийсэн хүүхэд" тест.
 *
 * Үндсэн thread нь нэг worker thread үүсгэнэ. Worker thread нь 2 хүүхэд
 * процесс fork() хийнэ. Linux-д эдгээр хүүхдийн real_parent нь WORKER
 * THREAD тул тэд worker-ийн children жагсаалтад ордог, leader-ийнх биш.
 * Зөвхөн leader-ийн children-ийг гүйдэг DFS тэднийг алгасна.
 *
 * Build:  gcc -O2 -pthread tests/thread_fork.c -o bd1-thrfork
 * Run:    ./bd1-thrfork [seconds]   (анхдагч 600)
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

static int seconds = 600;

static void *worker(void *arg)
{
	pid_t kids[2];

	(void)arg;
	prctl(PR_SET_NAME, "bd1-thrworker");
	for (int i = 0; i < 2; i++) {
		kids[i] = fork();
		if (kids[i] == 0) {
			prctl(PR_SET_NAME, "bd1-thrchild");
			sleep(seconds);
			_exit(0);
		}
	}
	for (int i = 0; i < 2; i++)
		waitpid(kids[i], NULL, 0);
	return NULL;
}

int main(int argc, char **argv)
{
	pthread_t t;

	if (argc > 1)
		seconds = atoi(argv[1]);
	if (seconds <= 0)
		seconds = 600;
	prctl(PR_SET_NAME, "bd1-thrfork");
	signal(SIGTERM, SIG_DFL);
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		perror("pthread_create");
		return 1;
	}
	pthread_join(t, NULL);
	return 0;
}
