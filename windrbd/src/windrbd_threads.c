/* Uncomment this if you want more debug output (disable for releases) */
/* #define DEBUG 1 */

#ifdef RELEASE
#ifdef DEBUG
#undef DEBUG
#endif
#endif

#include "drbd_windows.h"
#include "windrbd_threads.h"
#include <wdm.h>

/* Can not include both wdm.h and ntifs.h hence the prototype here: */

extern BOOLEAN KeSetKernelStackSwapEnable(BOOLEAN Enable);

static LIST_HEAD(thread_list);
static spinlock_t thread_list_lock;

	/* NO printk's in here. Used by printk internally, would loop. */
	/* Call this with thread_list_lock held. */

static pid_t next_pid;
static spinlock_t next_pid_lock;

static struct task_struct *__find_thread(PKTHREAD id)
{
	struct task_struct *t;

	list_for_each_entry(struct task_struct, t, &thread_list, list) {
		if (t->windows_thread == id)
			return t;
	}
	return NULL;
}


	/* NO printk's here, used internally by printk (via current). */
struct task_struct* windrbd_find_thread(PKTHREAD id)
{
	struct task_struct *t;
	KIRQL flags;

	spin_lock_irqsave(&thread_list_lock, flags);
	t = __find_thread(id);
	if (!t) {	/* TODO: ... */
		static struct task_struct g_dummy_current;
		t = &g_dummy_current;
		t->pid = 0;
		t->has_sig_event = FALSE;
		t->is_root = 0;
		strcpy(t->comm, "not_drbd_thread");
	}
	spin_unlock_irqrestore(&thread_list_lock, flags);

	return t;
}

void print_threads_in_rcu(void)
{
	static char buf[4096];
	int remaining_bytes = sizeof(buf)-1;
	int len;
	char *pos;
	struct task_struct *t;
	KIRQL flags;

	pos = buf;
        spin_lock_irqsave(&thread_list_lock, flags);
	list_for_each_entry(struct task_struct, t, &thread_list, list) {
		if (t->in_rcu) {
			len = snprintf(pos, remaining_bytes, "Thread %s holding rcu_read_lock\n", t->comm);
			pos+=len;
			remaining_bytes-=len;
		}
		if (remaining_bytes < 10)
			break;
	}
	spin_unlock_irqrestore(&thread_list_lock, flags);

	if (pos != buf)
		printk("Threads in rcu_lock\n%s", buf);
}

	/* Helper function to create and start windows kernel threads.
	 * If non-null, the kernel's PKTHREAD object is returned by
	 * reference in thread_object_p.
	 */

NTSTATUS windrbd_create_windows_thread(void (*threadfn)(void*), void *data, void **thread_object_p)
{
	HANDLE h;
	NTSTATUS status;
	int retries;

	retries = 0;
	while (1) {
	        status = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL, threadfn, data);
		if (NT_SUCCESS(status)) {
			if (retries > 0)
				printk("succeeded after %d retries\n", retries);
			break;
		}
		if (status != STATUS_INSUFFICIENT_RESOURCES)
			return status;

                if (retries % 10 == 0)
			printk(KERN_ERR "Could not start thread, status = %x, retrying ...\n", status);

                if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
			if (retries == 0)
				printk("cannot sleep now, busy looping\n");
		} else {
			msleep(100);
		}
		retries++;
	}

	if (thread_object_p)
	        status = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode, thread_object_p, NULL);

	ZwClose(h);
	return status;
}

NTSTATUS windrbd_cleanup_windows_thread(void *thread_object)
{
	NTSTATUS status;

        status = KeWaitForSingleObject(thread_object, Executive, KernelMode, FALSE, (PLARGE_INTEGER)NULL);

        if (!NT_SUCCESS(status)) {
                printk("KeWaitForSingleObject failed with status %x\n", status);
		return status;
	}
        ObDereferenceObject(thread_object);

	return STATUS_SUCCESS;
}

	/* Called by reply_reaper (see windrbd_netlink.c). We don't want
	 * two threads having reaping some resources and this thread was
	 * there first.
	 */

void windrbd_reap_threads(void)
{
	struct task_struct *t, *tn;
	KIRQL flags;
	LIST_HEAD(dead_list);

	INIT_LIST_HEAD(&dead_list);

	spin_lock_irqsave(&thread_list_lock, flags);
	list_for_each_entry_safe(struct task_struct, t, tn, &thread_list, list) {
		if (t->is_zombie) {
// printk("about to bury %p\n", t);
			list_del(&t->list);
			list_add(&t->list, &dead_list);
		}
	}
	spin_unlock_irqrestore(&thread_list_lock, flags);

	list_for_each_entry_safe(struct task_struct, t, tn, &dead_list, list) {
		windrbd_cleanup_windows_thread(t->windows_thread);
// printk("Buried %s thread\n", t->comm);

		list_del(&t->list);
		kfree(t);
	}
}

	/* To be called on shutdown. On driver unload all threads must
	 * be terminated, it is a BSOD if threads are remaining. So
	 * wait forever. printk still should work, so inform the user
 	 * that we are still alive waiting for threads to terminate.
	 */

void windrbd_reap_all_threads(void)
{
	struct task_struct *t;
	KIRQL flags;

	windrbd_reap_threads();

	while (!list_empty(&thread_list)) {
		printk("Still threads alive, waiting for them to terminate ...\n");

	/* TODO: printk will call current which also takes the lock. */
//		spin_lock_irqsave(&thread_list_lock, flags);
		list_for_each_entry(struct task_struct, t, &thread_list, list) {
			printk("    Thread %s still running ...\n", t->comm);
		}
//		spin_unlock_irqrestore(&thread_list_lock, flags);
		msleep(1000);
		windrbd_reap_threads();
	}
}

	/* We need this so we clean up the task struct. Linux appears
	 * to deref the task_struct on thread exit, we also should
	 * do so.
	 */

static void windrbd_thread_setup(void *targ)
{
	struct task_struct *t = targ;
	int ret;
	NTSTATUS status;

		/* Linux never swaps out kernel stack areas. This
		 * should fix a very rare list corruption in a
		 * wake_up() call (the list contained an element
		 * that was on a stack that was swapped out, causing
		 * list corruption).
		 */

	KeSetKernelStackSwapEnable(FALSE);

		/* t->windows_thread may be still invalid here, do not
		 * printk().
		 */

        status = KeWaitForSingleObject(&t->start_event, Executive, KernelMode, FALSE, (PLARGE_INTEGER)NULL);
        if (!NT_SUCCESS(status)) {
		printk("On waiting for start event: KeWaitForSingleObject failed with status %x\n", status);

		KeSetKernelStackSwapEnable(TRUE);
		return;
	}
	ret = t->threadfn(t->data);
	if (ret != 0)
		printk(KERN_WARNING "Thread %s returned non-zero exit status. Ignored, since Windows threads are void.\n", t->comm);

	if (t->wait_queue != NULL)
		printk("Warning: thread exiting with still wait_queue on it (%p).\n", t->wait_queue);
	if (t->wait_queue_entry != NULL)
		printk("Warning: thread exiting with still wait_queue_entry on it (%p).\n", t->wait_queue_entry);

	if (KeGetCurrentIrql() > PASSIVE_LEVEL)
		printk("Warning: IRQL is %d when exiting thread. System will posibly lockup.\n", KeGetCurrentIrql());

		/* According to Microsoft docs we must not exit a thread
		 * with stack swapping disabled, so enable it here again.
		 */
	KeSetKernelStackSwapEnable(TRUE);
// printk("exiting %p...\n", t);
	t->is_zombie = 1;
}

	/* Again, we try to be more close to the Linux kernel API.
	 * This really creates and starts the thread created earlier
	 * by kthread_create() as a windows kernel thread. If the
	 * start process should fail, -1 is returned (which is
	 * different from the Linux kernel API, sorry for that...)
	 * Else same as in Linux: 0: task is already running (yes,
	 * you can call this multiple times, but since there is no
	 * way to temporarily stop a windows kernel thread, always
	 * 0 is returned) or 1: task was started.
	 */

int wake_up_process(struct task_struct *t)
{
	KIRQL flags;
	NTSTATUS status;

	spin_lock_irqsave(&t->thread_started_lock, flags);
	if (t->thread_started) {
		spin_unlock_irqrestore(&t->thread_started_lock, flags);
		return 0;
	}
	t->thread_started = 1;
	spin_unlock_irqrestore(&t->thread_started_lock, flags);
	KeSetEvent(&t->start_event, 0, FALSE);

	return 1;
}

	/* Creates a new task_struct, but start the thread (by
	 * calling PsCreateSystemThread()). Thread will wait for
	 * start event which is signalled by wake_up_process(struct
	 * task_struct) later.
	 *
	 * If PsCreateSystemThread should fail this returns an
	 * ERR_PTR(-ENOMEM)
	 *
	 * This now 'emulates' Linux behaviour such that no changes
	 * to driver code should be neccessary (at least not in the
	 * DRBD code).
	 */

struct task_struct *kthread_create(int (*threadfn)(void *), void *data, const char *name, ...)
{
	struct task_struct *t;
	KIRQL flags;
	va_list args;
	NTSTATUS status;

	if ((t = kzalloc(sizeof(*t), GFP_KERNEL, 'DRBD')) == NULL)
		return ERR_PTR(-ENOMEM);

		/* The thread will be created later in wake_up_process(),
		 * since Windows doesn't know of threads that are stopped
		 * when created.
		 */

	t->windows_thread = NULL;
	t->threadfn = threadfn;
	t->data = data;
	t->thread_started = 0;
	t->is_zombie = 0;
	t->is_root = current->is_root;	/* inherit user ID */
	spin_lock_init(&t->thread_started_lock);

	/* TODO: this should be a NotificationEvent. UNIX Signals
	 * should remain signalled until explicitly removed
	 * by flush_signals(). Change it in the 1.2 branch.
	 */
	// KeInitializeEvent(&t->sig_event, NotificationEvent, FALSE);
	KeInitializeEvent(&t->sig_event, SynchronizationEvent, FALSE);
	KeInitializeEvent(&t->start_event, SynchronizationEvent, FALSE);
	t->has_sig_event = TRUE;
	t->sig = -1;

		/* ignore if string is too long */
	va_start(args, name);
	(void)  _vsnprintf_s(t->comm, sizeof(t->comm)-1, _TRUNCATE, name, args);
	va_end(args);
	t->comm[sizeof(t->comm)-1] = '\0';

	spin_lock_irqsave(&next_pid_lock, flags);
	next_pid++;
	t->pid = next_pid;
	spin_unlock_irqrestore(&next_pid_lock, flags);

	status = windrbd_create_windows_thread(windrbd_thread_setup, t, &t->windows_thread);
	if (status != STATUS_SUCCESS) {
		printk("Could not start thread %s, status is %x.\n", t->comm, status);
		kfree(t);
		return ERR_PTR(-ENOMEM);	/* or whatever */
	}

	spin_lock_irqsave(&thread_list_lock, flags);
	list_add(&t->list, &thread_list);
	spin_unlock_irqrestore(&thread_list_lock, flags);

	return t;
}

struct task_struct *kthread_run(int (*threadfn)(void *), void *data, const char *name)
{
	struct task_struct *k = kthread_create(threadfn, data, name);
	if (!IS_ERR(k))
		wake_up_process(k);
	return k;
}

	/* Use this to create a task_struct for a Windows thread
	 * This is needed so we can call wait_event_XXX functions
	 * within those threads.
	 */

struct task_struct *make_me_a_windrbd_thread(const char *name, ...)
{
	struct task_struct *t;
	KIRQL flags;
	va_list args;
	int i;
	NTSTATUS status;

	if ((t = kzalloc(sizeof(*t), GFP_KERNEL, 'DRBD')) == NULL)
		return ERR_PTR(-ENOMEM);

		/* The thread will be created later in wake_up_process(),
		 * since Windows doesn't know of threads that are stopped
		 * when created.
		 */

	t->windows_thread = KeGetCurrentThread();
	spin_lock_init(&t->thread_started_lock);

//	KeInitializeEvent(&t->sig_event, NotificationEvent, FALSE);
	KeInitializeEvent(&t->sig_event, SynchronizationEvent, FALSE);
	KeInitializeEvent(&t->start_event, SynchronizationEvent, FALSE);
	t->has_sig_event = TRUE;
	t->sig = -1;
	t->is_root = 0;

	va_start(args, name);
	i = _vsnprintf_s(t->comm, sizeof(t->comm)-1, _TRUNCATE, name, args);
	va_end(args);
	if (i == -1) {
		kfree(t);
		return ERR_PTR(-ERANGE);
	}

	spin_lock_irqsave(&next_pid_lock, flags);
	next_pid++;
	t->pid = next_pid;
	spin_unlock_irqrestore(&next_pid_lock, flags);

	KeSetKernelStackSwapEnable(FALSE);

	spin_lock_irqsave(&thread_list_lock, flags);
	list_add(&t->list, &thread_list);
	spin_unlock_irqrestore(&thread_list_lock, flags);

	return t;
}

	/* Call this when a thread returns to the calling Windows
	 * kernel function. This is mandatory since we enable
	 * stack swapping in here again.
	 */

void return_to_windows(struct task_struct *t)
{
	KIRQL flags;

	KeSetKernelStackSwapEnable(TRUE);

	spin_lock_irqsave(&thread_list_lock, flags);
	list_del(&t->list);
	spin_unlock_irqrestore(&thread_list_lock, flags);
	kfree(t);
}

bool is_windrbd_thread(struct task_struct *t)
{
	if (t == NULL)
		return false;

	return t->has_sig_event;
}

void windrbd_set_realtime_priority(struct task_struct *t)
{
	KPRIORITY old_priority;

	if (t == NULL)
		return;

	// old_priority = KeSetPriorityThread(t->windows_thread, HIGH_PRIORITY);
// printk("setting %s thread to LOW_REALTIME_PRIORITY\n", current->comm);
	old_priority = KeSetPriorityThread(t->windows_thread, LOW_REALTIME_PRIORITY);

// printk("old priority is %d\n", old_priority);
}

void sudo(void)
{
	if (is_windrbd_thread(current))
		current->is_root = 1;
}

void init_windrbd_threads(void)
{
        spin_lock_init(&next_pid_lock);
        spin_lock_init(&thread_list_lock);
	thread_list_lock.printk_lock = 1;
        INIT_LIST_HEAD(&thread_list);
}

