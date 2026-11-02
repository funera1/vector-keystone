#include "edge/edge_call.h"
#include "host/keystone.h"
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sched.h>
#include <limits.h>
#include <unistd.h>
#include <iostream>
#include <numa.h>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>

using namespace Keystone;


#define DEFAULT_INTERVAL 		1000
#define DEFAULT_DISTANCE 		500



#define MSEC_PER_SEC			1000
#define USEC_PER_SEC			1000000
#define NSEC_PER_SEC			1000000000
#define USEC_TO_NSEC(u)			((u) * 1000)
#define USEC_TO_SEC(u)			((u) / USEC_PER_SEC)
#define NSEC_TO_USEC(n)			((n) / 1000)
#define SEC_TO_NSEC(s)			((s) * NSEC_PER_SEC)
#define SEC_TO_USEC(s)			((s) * USEC_PER_SEC)

#define CLOCK_FREQ 4000000UL    // VisionFive2/JH7110 timebase is 4MHz
#define NSEC_PER_CYCLE (NSEC_PER_SEC / CLOCK_FREQ)

#define sigev_notify_thread_id 	_sigev_un._tid


#define _STR(x) 				#x
#define STR(x) 					_STR(x)

#define MAX_PATH 				256

#define HIST_MAX				1000000

#define MODE_CYCLIC				0
#define MODE_CLOCK_NANOSLEEP	1
#define MODE_SYS_ITIMER			2
#define MODE_SYS_NANOSLEEP		3
#define MODE_SYS_OFFSET			2
#define TIMER_RELTIME			0

#define TRACEBUFSIZ  			1024
#define DEFAULT_INTERVAL 		1000

#define MAX_COMMAND_LINE 		4096
#define MAX_TS_SIZE 			64

#ifndef SCHED_NORMAL
#define SCHED_NORMAL SCHED_OTHER
#endif

#define HSET_PRINT_SUM			1
#define HSET_PRINT_JSON			2

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Must be power of 2 ! */
#define VALBUF_SIZE				16384

static int numa = 0;

static int aligned = 0;
static int secaligned = 0;
static int offset = 0;
static int duration = 0;
static int shutdown;
static int use_nsecs = 0;
static int force_sched_other;
static int check_clock_resolution;
static int oscope_reduction = 1;
static int verbose = 0;
static int no_enclave = 0;
static int lockall = 0;
static int priospread = 0;

static int tracelimit = 0;
static int trace_marker = 0;
static int histogram = 0;
static int histofall = 0;
static int refresh_on_max;
static int ct_debug;
static int use_fifo = 0;
static pthread_t fifo_threadid;
static int laptop = 0;
static int power_management = 0;
static int use_histfile = 0;
static int delay = 0;  /* delay in seconds before starting test */

static char *fileprefix;

#ifdef ARCH_HAS_SMI_COUNTER
static int smi = 0;
#else
#define smi	0
#endif

static pthread_cond_t refresh_on_max_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t refresh_on_max_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t break_thread_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t break_thread_id = 0;
static uint64_t break_thread_value = 0;

static pthread_barrier_t align_barr;
static pthread_barrier_t globalt_barr;
static struct timespec globalt;

static int latency_target_fd = -1;
static int32_t latency_target_value = 0;

static int deepest_idle_state = -2;





static int tracemark_fd = -1;
static int trace_fd = -1;
static __thread char tracebuf[TRACEBUFSIZ];

static char fifopath[MAX_PATH];
static char histfile[MAX_PATH];
static char jsonfile[MAX_PATH];

static char debugfileprefix[MAX_PATH];

static int rstat_fd = -1;

#define SHM_BUF_SIZE 19
static char shm_name[SHM_BUF_SIZE];

static char test_cmdline[MAX_COMMAND_LINE];
static char ts_start[MAX_TS_SIZE];


struct enclave_args {
	const char* eapppath;
	const char* runtimepath;
	const char* loaderpath;
};


struct thread_param {
	int prio;
	int policy;
	int mode;
	int timermode;
	int signal;
	int clock;
	unsigned long max_cycles;
	struct thread_stat *stats;
	int bufmsk;
	unsigned long interval;
	int cpu;
	int node;
	int tnum;
	int msr_fd;
	struct enclave_args *enc_args;
};

struct thread_stat {
	unsigned long cycles;
	unsigned long cyclesread;
	long min;
	long max;
	long act;
	double avg;
	long *values;
	long *smis;
	struct histogram *hist;
	pthread_t thread;
	int threadstarted;
	int tid;
	long reduce;
	long redmax;
	long cycleofmax;
	unsigned long smi_count;
};

static pthread_mutex_t trigger_lock = PTHREAD_MUTEX_INITIALIZER;

static int trigger = 0;	/* Record spikes > trigger, 0 means don't record */
static int trigger_list_size = 1024;	/* Number of list nodes */

/* Info to store when the diff is greater than the trigger */
struct thread_trigger {
	int cpu;
	int tnum;	/* thread number */
	int64_t  ts;	/* time-stamp */
	int diff;
	struct thread_trigger *next;
};

struct thread_trigger *head = NULL;
struct thread_trigger *tail = NULL;
struct thread_trigger *current = NULL;
static int spikes;

struct histogram {
	unsigned long *buckets;
	unsigned long width;		// interval covered by one bucket
	unsigned long num;		// number of buckets
	unsigned long events;		// number of events logged

	unsigned long *oflows;		// events when overflow happened
	unsigned long oflow_bufsize;	// number of overflows that can be logged
	unsigned long oflow_count;	// number of events that overflowed
	uint64_t oflow_magnitude;	// sum of how many buckets overflowed by
};

struct histoset {
	struct histogram *histos;	// Group of related histograms (e.g. per cpu)
	struct histogram *sum;		// Accumulates events from all histos
	unsigned long num_histos;	// Not including sum
	unsigned long num_buckets;
};

#define HIST_OVERFLOW		1
#define HIST_OVERFLOW_MAG	2
#define HIST_OVERFLOW_LOG	4

static struct thread_param **parameters;
static struct thread_stat **statistics;
static struct enclave_args **enc_args;
static struct histoset hset;


typedef unsigned long long u64;
typedef unsigned int u32;
typedef int s32;


struct sched_data {
	u64 runtime_us;
	u64 deadline_us;

	int bufmsk;

	struct thread_stat stat;

	char buff[BUFSIZ+1];
};

static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
	int64_t diff = USEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec) / 1000;
	return diff;
}

static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2)
{
	int64_t diff;
	diff = NSEC_PER_SEC * (int64_t)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec);
	return diff;
}

static inline int64_t calctime(struct timespec t)
{
	int64_t time;
	time = USEC_PER_SEC * t.tv_sec;
	time += ((int) t.tv_nsec) / 1000;
	return time;
}

static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static inline void rdtimespec(unsigned long rdtime, struct timespec *ts){
	ts->tv_sec = rdtime / CLOCK_FREQ;
    ts->tv_nsec = (rdtime % CLOCK_FREQ) * NSEC_PER_CYCLE;
}

static inline void get_rdtime(struct timespec *ts){
	unsigned long rdtime;
	__asm__ __volatile__("rdtime %0" : "=r"(rdtime));
    rdtimespec(rdtime, ts);
}

static inline int tsgreater(struct timespec *a, struct timespec *b)
{
	return ((a->tv_sec > b->tv_sec) ||
		(a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

void err_doit(int err, const char *fmt, va_list ap)
{
	vfprintf(stderr, fmt, ap);
	if (err)
		fprintf(stderr, ": %s\n", strerror(err));
	return;
}

void debug(int enable, char *fmt, ...)
{
	if (enable) {
		va_list ap;

		va_start(ap, fmt);
		fputs("DEBUG: ", stderr);
		err_doit(0, fmt, ap);
		va_end(ap);
	}
}

void info(int enable, char *fmt, ...)
{
	if (enable) {
		va_list ap;

		va_start(ap, fmt);
		fputs("INFO: ", stderr);
		err_doit(0, fmt, ap);
		va_end(ap);
	}
}

void warn(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("WARN: ", stderr);
	err_doit(0, fmt, ap);
	va_end(ap);
}

void fatal(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("FATAL: ", stderr);
	err_doit(0, fmt, ap);
	va_end(ap);
	exit(EXIT_FAILURE);
}

void err_exit(int err, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(err, fmt, ap);
	va_end(ap);
	exit(err);
}

void err_msg(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, fmt, ap);
	va_end(ap);
	return;
}

void err_msg_n(int err, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(err, fmt, ap);
	va_end(ap);
	return;
}

void err_quit(char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	err_doit(0, fmt, ap);
	va_end(ap);
	exit(1);
}

pid_t gettid(void)
{
	return syscall(SYS_gettid);
}

#ifdef HAVE_LIBCPUPOWER_SUPPORT
static unsigned int **saved_cpu_idle_disable_state;
static size_t saved_cpu_idle_disable_state_alloc_ctr;

/*
 * save_cpu_idle_state_disable - save disable for all idle states of a cpu
 *
 * Saves the current disable of all idle states of a cpu, to be subsequently
 * restored via restore_cpu_idle_disable_state.
 *
 * Return: idle state count on success, negative on error
 */
static int save_cpu_idle_disable_state(unsigned int cpu)
{
	unsigned int nr_states;
	unsigned int state;
	int disabled;
	int nr_cpus;

	nr_states = cpuidle_state_count(cpu);

	if (nr_states == 0)
		return 0;

	if (saved_cpu_idle_disable_state == NULL) {
		nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
		saved_cpu_idle_disable_state = calloc(nr_cpus, sizeof(unsigned int *));
		if (!saved_cpu_idle_disable_state)
			return -1;
	}

	saved_cpu_idle_disable_state[cpu] = calloc(nr_states, sizeof(unsigned int));
	if (!saved_cpu_idle_disable_state[cpu])
		return -1;
	saved_cpu_idle_disable_state_alloc_ctr++;

	for (state = 0; state < nr_states; state++) {
		disabled = cpuidle_is_state_disabled(cpu, state);
		if (disabled < 0)
			return disabled;
		saved_cpu_idle_disable_state[cpu][state] = disabled;
	}

	return nr_states;
}

/*
 * restore_cpu_idle_disable_state - restore disable for all idle states of a cpu
 *
 * Restores the current disable state of all idle states of a cpu that was
 * previously saved by save_cpu_idle_disable_state.
 *
 * Return: idle state count on success, negative on error
 */
static int restore_cpu_idle_disable_state(unsigned int cpu)
{
	unsigned int nr_states;
	unsigned int state;
	int disabled;
	int result;

	nr_states = cpuidle_state_count(cpu);

	if (nr_states == 0)
		return 0;

	if (!saved_cpu_idle_disable_state)
		return -1;

	for (state = 0; state < nr_states; state++) {
		if (!saved_cpu_idle_disable_state[cpu])
			return -1;
		disabled = saved_cpu_idle_disable_state[cpu][state];
		result = cpuidle_state_disable(cpu, state, disabled);
		if (result < 0)
			return result;
	}

	free(saved_cpu_idle_disable_state[cpu]);
	saved_cpu_idle_disable_state[cpu] = NULL;
	saved_cpu_idle_disable_state_alloc_ctr--;
	if (saved_cpu_idle_disable_state_alloc_ctr == 0) {
		free(saved_cpu_idle_disable_state);
		saved_cpu_idle_disable_state = NULL;
	}

	return nr_states;
}

/*
 * free_cpu_idle_disable_states - free saved idle state disable for all cpus
 *
 * Frees the memory used for storing cpu idle state disable for all cpus
 * and states.
 *
 * Normally, the memory is freed automatically in
 * restore_cpu_idle_disable_state; this is mostly for cleaning up after an
 * error.
 */
static void free_cpu_idle_disable_states(void)
{
	int cpu;
	int nr_cpus;

	if (!saved_cpu_idle_disable_state)
		return;

	nr_cpus = sysconf(_SC_NPROCESSORS_CONF);

	for (cpu = 0; cpu < nr_cpus; cpu++) {
		free(saved_cpu_idle_disable_state[cpu]);
		saved_cpu_idle_disable_state[cpu] = NULL;
	}

	free(saved_cpu_idle_disable_state);
	saved_cpu_idle_disable_state = NULL;
}

/*
 * set_deepest_cpu_idle_state - limit idle state of cpu
 *
 * Disables all idle states deeper than the one given in
 * deepest_state (assuming states with higher number are deeper).
 *
 * This is used to reduce the exit from idle latency. Unlike
 * set_cpu_dma_latency, it can disable idle states per cpu.
 *
 * Return: idle state count on success, negative on error
 */
static int set_deepest_cpu_idle_state(unsigned int cpu, unsigned int deepest_state)
{
	unsigned int nr_states;
	unsigned int state;
	int result;

	nr_states = cpuidle_state_count(cpu);

	for (state = deepest_state + 1; state < nr_states; state++) {
		result = cpuidle_state_disable(cpu, state, 1);
		if (result < 0)
			return result;
	}

	return nr_states;
}

static inline int have_libcpupower_support(void) { return 1; }
#else
static inline int save_cpu_idle_disable_state(__attribute__((unused)) unsigned int cpu) { return -1; }
static inline int restore_cpu_idle_disable_state(__attribute__((unused)) unsigned int cpu) { return -1; }
static inline void free_cpu_idle_disable_states(void) { }
static inline int set_deepest_cpu_idle_state(__attribute__((unused)) unsigned int cpu,
											 __attribute__((unused)) unsigned int state) { return -1; }
static inline int have_libcpupower_support(void) { return 0; }
#endif /* HAVE_LIBCPUPOWER_SUPPORT */

enum {
	ERROR_GENERAL	= -1,
	ERROR_NOTFOUND	= -2,
};

static int raise_soft_prio(int policy, const struct sched_param *param)
{
	int err;
	int policy_max;
	int soft_max;
	int hard_max;
	int prio;
	struct rlimit rlim;

	prio = param->sched_priority;

	policy_max = sched_get_priority_max(policy);
	if (policy_max == -1) {
		err = errno;
		err_msg("WARN: no such policy\n");
		return err;
	}

	err = getrlimit(RLIMIT_RTPRIO, &rlim);
	if (err) {
		err = errno;
		err_msg_n(err, "WARN: getrlimit failed");
		return err;
	}

	soft_max = (rlim.rlim_cur == RLIM_INFINITY) ? (unsigned int)policy_max
						    : rlim.rlim_cur;
	hard_max = (rlim.rlim_max == RLIM_INFINITY) ? (unsigned int)policy_max
						    : rlim.rlim_max;

	if (prio > soft_max && prio <= hard_max) {
		rlim.rlim_cur = prio;
		err = setrlimit(RLIMIT_RTPRIO, &rlim);
		if (err) {
			err = errno;
			err_msg_n(err, "WARN: setrlimit failed");
		}
	} else {
		err = -1;
	}

	return err;
}

static int trigger_init()
{
	int i;
	int size = trigger_list_size;
	struct thread_trigger *trig = NULL;
	for(i=0; i<size; i++) {
		trig = (struct thread_trigger*) malloc(sizeof(struct thread_trigger));
		if (trig != NULL) {
			if  (head == NULL) {
				head = trig;
				tail = trig;
			} else {
				tail->next = trig;
				tail = trig;
			}
			trig->tnum = i;
			trig->next = NULL;
		} else {
			return -1;
		}
	}
	current = head;
	return 0;
}

static void trigger_print()
{
	struct thread_trigger *trig = head;
	char *fmt = "T:%2d Spike:%8ld: TS: %12ld\n";

	if (current == head)
		return;
	printf("\n");
	while (trig->next != current) {
		fprintf(stdout, fmt,  trig->tnum, trig->diff, trig->ts);
		trig = trig->next;
	}
		fprintf(stdout, fmt,  trig->tnum, trig->diff, trig->ts);
		printf("spikes = %d\n\n", spikes);
}

static void trigger_update(struct thread_param *par, int diff, int64_t ts)
{
	pthread_mutex_lock(&trigger_lock);
	if (current != NULL) {
		current->tnum = par->tnum;
		current->ts = ts;
		current->diff = diff;
		current = current->next;
	}
	spikes++;
	pthread_mutex_unlock(&trigger_lock);
}


int hist_init(struct histogram *h, unsigned long width, unsigned long num)
{
	memset(h, 0, sizeof(*h));
	h->width = width;
	h->num = num;

	h->buckets = (unsigned long *) calloc(num, sizeof(unsigned long));
	if (!h->buckets)
		return -ENOMEM;

	return 0;
}

int hist_init_oflow(struct histogram *h, unsigned long num)
{
	h->oflow_bufsize = num;
	h->oflows = (unsigned long *) calloc(num, sizeof(unsigned long));
	if (!h->oflows)
		return -ENOMEM;

	return 0;
}

void hist_destroy(struct histogram *h)
{
	free(h->oflows);
	h->oflows = NULL;
	free(h->buckets);
	h->buckets = NULL;
}

int hist_sample(struct histogram *h, uint64_t sample)
{
	unsigned long bucket = sample / h->width;
	unsigned long extra;
	unsigned long event = h->events++;
	int ret;

	if (bucket < h->num) {
		h->buckets[bucket]++;
		return 0;
	}

	ret = HIST_OVERFLOW;
	extra = bucket - h->num;
	if (h->oflow_magnitude + extra > h->oflow_magnitude)
		h->oflow_magnitude += extra;
	else
		ret |= HIST_OVERFLOW_MAG;

	if (h->oflows) {
		if (h->oflow_count < h->oflow_bufsize)
			h->oflows[h->oflow_count] = event;
		else
			ret |= HIST_OVERFLOW_LOG;
	}

	h->oflow_count++;
	return ret;
}

void hset_destroy(struct histoset *hs)
{
	unsigned long i;

	if (hs->histos) {
		for (i = 0; i < hs->num_histos; i++)
			hist_destroy(&hs->histos[i]);
	}

	free(hs->histos);
	hs->histos = NULL;
}

void hset_print_bucket(struct histoset *hs, FILE *f, const char *pre,
		       unsigned long bucket, unsigned long flags)
{
	unsigned long long sum = 0;
	unsigned long i;

	if (bucket >= hs->num_buckets)
		return;

	for (i = 0; i < hs->num_histos; i++)
		sum += hs->histos[i].buckets[bucket];

	if (sum == 0)
		return;
	if (pre)
		fprintf(f, "%s", pre);

	for (i = 0; i < hs->num_histos; i++) {
		unsigned long val = hs->histos[i].buckets[bucket];

		if (i != 0)
			fprintf(f, "\t");
		fprintf(f, "%06lu", val);
	}

	if (flags & HSET_PRINT_SUM)
		fprintf(f, "\t%06llu", sum);

	fprintf(f, "\n");
}

void hist_print_json(struct histogram *h, FILE *f)
{
	unsigned long i;
	bool comma = false;

	for (i = 0; i < h->num; i++) {
		unsigned long val = h->buckets[i];

		if (val != 0) {
			if (comma)
				fprintf(f, ",");
			fprintf(f, "\n        \"%lu\": %lu", i, val);
			comma = true;
		}
	}

	fprintf(f, "\n");
}


int hset_init(struct histoset *hs, unsigned long num_histos,
	      unsigned long bucket_width, unsigned long num_buckets,
	      unsigned long overflow)
{
	unsigned long i;

	if (num_histos == 0)
		return -EINVAL;

	hs->num_histos = num_histos;
	hs->num_buckets = num_buckets;
	hs->histos = (struct histogram *) calloc(num_histos, sizeof(struct histogram));
	if (!hs->histos)
		return -ENOMEM;

	for (i = 0; i < num_histos; i++) {
		if (hist_init(&hs->histos[i], bucket_width, num_buckets))
			goto fail;
		if (overflow && hist_init_oflow(&hs->histos[i], overflow))
			goto fail;
	}

	return 0;

fail:
	hset_destroy(hs);
	return -ENOMEM;
}



static int setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
	int err = 0;

try_again:
	err = sched_setscheduler(pid, policy, param);
	if (err) {
		err = errno;
		if (err == EPERM) {
			int err1;
			err1 = raise_soft_prio(policy, param);
			if (!err1)
				goto try_again;
		}
	}

	return err;
}

/*
 * After this function is called, affinity_mask is the intersection of
 * the user supplied affinity mask and the affinity mask from the run
 * time environment
 */
static void use_current_cpuset(int max_cpus, struct bitmask *cpumask)
{
	struct bitmask *curmask;
	int i;

	curmask = numa_allocate_cpumask();
	numa_sched_getaffinity(getpid(), curmask);

	/*
	 * Clear bits that are not set in both the cpuset from the
	 * environment, and in the user specified affinity.
	 */
	for (i = 0; i < max_cpus; i++) {
		if ((!numa_bitmask_isbitset(cpumask, i)) ||
		    (!numa_bitmask_isbitset(curmask, i)))
			numa_bitmask_clearbit(cpumask, i);
	}

	numa_bitmask_free(curmask);
}

int parse_cpumask(char *str, int max_cpus, struct bitmask **cpumask)
{
	struct bitmask *mask;

	mask = numa_parse_cpustring_all(str);
	if (!mask)
		return -ENOMEM;

	if (numa_bitmask_weight(mask) == 0) {
		numa_bitmask_free(mask);
		*cpumask = NULL;
		return 0;
	}

	if (strchr(str, '!') != NULL || strchr(str, '+') != NULL)
		use_current_cpuset(max_cpus, mask);
	*cpumask = mask;

	return 0;
}

void tracemark(char *fmt, ...)
{
	va_list ap;
	int len;
	
	if (tracemark_fd < 0 || trace_fd < 0)
		return;

	va_start(ap, fmt);
	len = vsnprintf(tracebuf, TRACEBUFSIZ, fmt, ap);
	va_end(ap);

	write(tracemark_fd, tracebuf, len);

	write(trace_fd, "0\n", 2);
}


#ifdef ARCH_HAS_SMI_COUNTER
static int open_msr_file(int cpu)
{
	int fd;
	char pathname[32];

	/* SMI needs thread affinity */
	sprintf(pathname, "/dev/cpu/%d/msr", cpu);
	fd = open(pathname, O_RDONLY);
	if (fd < 0)
		warn("%s open failed, try chown or chmod +r "
		       "/dev/cpu/*/msr, or run as root\n", pathname);

	return fd;
}

static int get_msr(int fd, off_t offset, unsigned long long *msr)
{
	ssize_t retval;

	retval = pread(fd, msr, sizeof *msr, offset);

	if (retval != sizeof *msr)
		return 1;

	return 0;
}

static int get_smi_counter(int fd, unsigned long *counter)
{
	int retval;
	unsigned long long msr;

	retval = get_msr(fd, MSR_SMI_COUNT, &msr);
	if (retval)
		return retval;

	*counter = (unsigned long) (msr & MSR_SMI_COUNT_MASK);

	return 0;
}

#include <cpuid.h>

/* Guess if the CPU has an SMI counter in the model-specific registers (MSR).
 *
 * This is true for Intel x86 CPUs after Nehalem (2008). However, it's not
 * possible to detect the feature directly, (or at least, turbostat.c doesn't
 * know how to do it either) so we assume it's true for all x86 CPUs.
 * Previously, this function had an explicit allowlist, which required updates
 * every time a new CPU generation was released.
 */
static int has_smi_counter(void)
{
	unsigned int ebx, ecx, edx, max_level;
	unsigned int fms, family, model;

	fms = family = model = ebx = ecx = edx = 0;

	__get_cpuid(0, &max_level, &ebx, &ecx, &edx);

	/* check genuine intel */
	if (!(ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e))
		return 0;

	__get_cpuid(1, &fms, &ebx, &ecx, &edx);
	family = (fms >> 8) & 0xf;

	if (family != 6)
		return 0;

	/* no MSR */
	if (!(edx & (1 << 5)))
		return 0;

	return 1;
}
#else
static int open_msr_file(int cpu)
{
	return -1;
}

static int get_smi_counter(int fd, unsigned long *counter)
{
	return 1;
}
static int has_smi_counter(void)
{
	return 0;
}
#endif


static void rt_numa_set_numa_run_on_node(int node, int cpu)
{
	int res;
	res = numa_run_on_node(node);
	if (res)
		warn("Could not set NUMA node %d for thread %d: %s\n",
				node, cpu, strerror(errno));
	return;
}


static void *enclave_thread(void *param)
{
	 
	struct thread_param *par = (struct thread_param *) param;
	struct sched_param schedp;
	struct sigevent sigev;
	sigset_t sigset;
	timer_t timer;
	struct timespec now, next, now_rd, next_rd, interval, stop = { 0 };
	struct itimerval itimer;
	struct itimerspec tspec;
	struct thread_stat *stat = par->stats;
	int stopped = 0;
	cpu_set_t mask;
	pthread_t thread;
	unsigned long smi_now, smi_old = 0;
	

	struct enclave_args *enc = par->enc_args;
	uintptr_t rdtime;
	Params enc_params;
	enc_params.setFreeMemSize(256 * 1024);
	enc_params.setUntrustedSize(256 * 1024);

	/* if we're running in numa mode, set our memory node */
	if (par->node != -1)
		rt_numa_set_numa_run_on_node(par->node, par->cpu);

  	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		thread = pthread_self();
		if (pthread_setaffinity_np(thread, sizeof(mask), &mask) != 0)
			warn("Could not set CPU affinity to CPU #%d\n",par->cpu);
			
	}

  	interval.tv_sec = par->interval / USEC_PER_SEC;
	interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;

  	stat->tid = gettid();

	sigemptyset(&sigset);
	sigaddset(&sigset, par->signal);
	sigprocmask(SIG_BLOCK, &sigset, NULL);
	
	if (par->mode == MODE_CYCLIC) {
		sigev.sigev_notify = SIGEV_THREAD_ID | SIGEV_SIGNAL;
		sigev.sigev_signo = par->signal;
		sigev.sigev_notify_thread_id = stat->tid;
		timer_create(par->clock, &sigev, &timer);
		tspec.it_interval = interval;
	}

  	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->prio;
  	if (setscheduler(0, par->policy, &schedp))
		fatal("timerthread%d: failed to set priority to %d\n",par->cpu, par->prio);

	if (smi) {
		par->msr_fd = open_msr_file(par->cpu);
		if (par->msr_fd < 0)
			fatal("Could not open MSR interface, errno: %d\n",
				errno);
		/* get current smi count to use as base value */
		if (get_smi_counter(par->msr_fd, &smi_old))
			fatal("Could not read SMI counter, errno: %d\n",
				par->cpu, errno);
	}

	/* Get current time */
	if (aligned || secaligned) {
		pthread_barrier_wait(&globalt_barr);
		if (par->tnum == 0) {
			clock_gettime(par->clock, &globalt);
			if (secaligned) {
				/* Ensure that the thread start timestamp is not
				 * in the past */
				if (globalt.tv_nsec > 900000000)
					globalt.tv_sec += 2;
				else
					globalt.tv_sec++;
				globalt.tv_nsec = 0;
			}
		}
		pthread_barrier_wait(&align_barr);
		now = globalt;
		if (offset) {
			if (aligned)
				now.tv_nsec += offset * par->tnum;
			else
				now.tv_nsec += offset;
			tsnorm(&now);
		}
	} else
		clock_gettime(par->clock, &now);

  	next = now;
	next.tv_sec += interval.tv_sec;
	next.tv_nsec += interval.tv_nsec;
	tsnorm(&next);

	//Get time from rdtime for stats
	get_rdtime(&now_rd);
	next_rd = now_rd;
	next_rd.tv_sec += interval.tv_sec;
	next_rd.tv_nsec += interval.tv_nsec;
	tsnorm(&next_rd);

	if (duration) {
		stop = now;
		stop.tv_sec += duration;
	}
	if (par->mode == MODE_CYCLIC) {
		if (par->timermode == TIMER_ABSTIME)
			tspec.it_value = next;
		else
			tspec.it_value = interval;
		timer_settime(timer, par->timermode, &tspec, NULL);
	}

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_interval.tv_sec = interval.tv_sec;
		itimer.it_interval.tv_usec = interval.tv_nsec / 1000;
		itimer.it_value = itimer.it_interval;
		setitimer(ITIMER_REAL, &itimer, NULL);
	}

  	stat->threadstarted++;

	/* Apply initial delay before starting the test (high priority) */
	if (delay > 0 && par->tnum == 0) {
		if (verbose)
			printf("Delaying %d seconds before test start\n", delay);
		for (int i = 0; i < delay; i++) {
			struct timespec delay_ts;
			clock_gettime(CLOCK_MONOTONIC, &delay_ts);
			delay_ts.tv_sec += 1;
			/* Use TIMER_ABSTIME for high priority sleep - wakes up at exact time */
			if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &delay_ts, NULL) == 0) {
				if (verbose)
					printf("Delay progress: %d/%d seconds\n", i+1, delay);
			}
		}
		/* Synchronize all threads after delay completes */
		pthread_barrier_wait(&globalt_barr);
	}

  	while(!shutdown){

		uint64_t diff;
		unsigned long diff_smi = 0;
		int sigs, ret;


		/* Wait for next period */
		switch (par->mode) {
		case MODE_CYCLIC:
		case MODE_SYS_ITIMER:
			if (sigwait(&sigset, &sigs) < 0)
				goto out;
			break;

		case MODE_CLOCK_NANOSLEEP:
			if (par->timermode == TIMER_ABSTIME) {
				ret = clock_nanosleep(par->clock, TIMER_ABSTIME,
						      &next, NULL);
				if (ret != 0) {
					if (ret != EINTR)
						warn("clock_nanosleep failed. errno: %d\n", errno);
					goto out;
				}
			} else {
				ret = clock_gettime(par->clock, &now);
				if (ret != 0) {
					if (ret != EINTR)
						warn("clock_gettime() failed: %s", strerror(errno));
					goto out;
				}
				ret = clock_nanosleep(par->clock,
					TIMER_RELTIME, &interval, NULL);
				if (ret != 0) {
					if (ret != EINTR)
						warn("clock_nanosleep() failed. errno: %d\n", errno);
					goto out;
				}
				next.tv_sec = now.tv_sec + interval.tv_sec;
				next.tv_nsec = now.tv_nsec + interval.tv_nsec;
				tsnorm(&next);
			}
			break;

		case MODE_SYS_NANOSLEEP:
			ret = clock_gettime(par->clock, &now);
			if (ret != 0) {
				if (ret != EINTR)
					warn("clock_gettime() failed: errno %d\n", errno);
				goto out;
			}
			if (nanosleep(&interval, NULL)) {
				if (errno != EINTR)
					warn("nanosleep failed. errno: %d\n",
					     errno);
				goto out;
			}
			next.tv_sec = now.tv_sec + interval.tv_sec;
			next.tv_nsec = now.tv_nsec + interval.tv_nsec;
			tsnorm(&next);
			break;
		}


		if (!no_enclave){
			Enclave enclave;
			enclave.init(enc->eapppath, enc->runtimepath, enc->loaderpath, enc_params);
			enclave.registerOcallDispatch(incoming_call_dispatch);
			edge_call_init_internals(
				(uintptr_t)enclave.getSharedBuffer(), enclave.getSharedBufferSize());
			enclave.run(&rdtime);
			rdtimespec((unsigned long) rdtime, &now_rd);
		} else {
			get_rdtime(&now_rd);
		}

		ret = clock_gettime(par->clock, &now);
		if (ret != 0) {
			if (ret != EINTR)
				warn("clock_gettime() failed. errno: %d\n",
				     errno);
			goto out;
		}

		if (smi) {
			if (get_smi_counter(par->msr_fd, &smi_now)) {
				warn("Could not read SMI counter, errno: %d\n",
					par->cpu, errno);
				goto out;
			}
			diff_smi = smi_now - smi_old;
			stat->smi_count += diff_smi;
			smi_old = smi_now;
		}

		if (use_nsecs)
			diff = calcdiff_ns(now_rd, next_rd);
		else
			diff = calcdiff(now_rd, next_rd);
		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max) {
			stat->max = diff;
			if (refresh_on_max)
				pthread_cond_signal(&refresh_on_max_cond);
		}
		stat->avg += (double) diff;

		if (trigger && (diff > trigger))
			trigger_update(par, diff, calctime(now_rd));

		if (duration && (calcdiff(now, stop) >= 0))
			shutdown++;

		if (!stopped && tracelimit && (diff > tracelimit)) {
			stopped++;
			shutdown++;
			pthread_mutex_lock(&break_thread_id_lock);
			if (break_thread_id == 0) {
				break_thread_id = stat->tid;
				tracemark("hit latency threshold (%llu > %d)",
					  (unsigned long long) diff, tracelimit);
				break_thread_value = diff;
			}
			pthread_mutex_unlock(&break_thread_id_lock);
		}
		stat->act = diff;

		if (par->bufmsk) {
			stat->values[stat->cycles & par->bufmsk] = diff;
			if (smi)
				stat->smis[stat->cycles & par->bufmsk] = diff_smi;
		}

		/* Update the histogram */
		if (histogram)
			hist_sample(stat->hist, diff);

		stat->cycles++;

		next.tv_sec += interval.tv_sec;
		next.tv_nsec += interval.tv_nsec;
		if (par->mode == MODE_CYCLIC) {
			int overrun_count = timer_getoverrun(timer);
			next.tv_sec += overrun_count * interval.tv_sec;
			next.tv_nsec += overrun_count * interval.tv_nsec;
		}
		tsnorm(&next);

		while (tsgreater(&now, &next)) {
			next.tv_sec += interval.tv_sec;
			next.tv_nsec += interval.tv_nsec;
			tsnorm(&next);
		}

		next_rd.tv_sec += interval.tv_sec;
		next_rd.tv_nsec += interval.tv_nsec;
		if (par->mode == MODE_CYCLIC) {
			int overrun_count = timer_getoverrun(timer);
			next_rd.tv_sec += overrun_count * interval.tv_sec;
			next_rd.tv_nsec += overrun_count * interval.tv_nsec;
		}
		tsnorm(&next_rd);

		while (tsgreater(&now_rd, &next_rd)) {
			next_rd.tv_sec += interval.tv_sec;
			next_rd.tv_nsec += interval.tv_nsec;
			tsnorm(&next_rd);
		}

		if (par->max_cycles && par->max_cycles == stat->cycles)
			break;

  	}

out:
	if (refresh_on_max) {
		pthread_mutex_lock(&refresh_on_max_lock);
		/* We could reach here with both shutdown and allstopped unset (0).
		 * Set shutdown with synchronization to notify the main
		 * thread not to be blocked when it should exit.
		 */
		shutdown++;
		pthread_cond_signal(&refresh_on_max_cond);
		pthread_mutex_unlock(&refresh_on_max_lock);
	}

	if (par->mode == MODE_CYCLIC)
		timer_delete(timer);

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_value.tv_sec = 0;
		itimer.it_value.tv_usec = 0;
		itimer.it_interval.tv_sec = 0;
		itimer.it_interval.tv_usec = 0;
		setitimer(ITIMER_REAL, &itimer, NULL);
	}

	/* close msr file */
	if (smi)
		close(par->msr_fd);
	/* switch to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);
	stat->threadstarted = -1;

	return nullptr;

}

static void *rt_numa_numa_alloc_onnode(size_t size, int node, int cpu)
{
	void *stack;
	stack = numa_alloc_onnode(size, node);
	if (stack == NULL)
		fatal("failed to allocate %d bytes on node %d for cpu %d\n",
				size, node, cpu);
	return stack;
}
	
int parse_time_string(char *val)
{
	char *end;
	int t = strtol(val, &end, 10);
	if (end) {
		switch (*end) {
		case 'm':
		case 'M':
			t *= 60;
			break;

		case 'h':
		case 'H':
			t *= 60*60;
			break;

		case 'd':
		case 'D':
			t *= 24*60*60;
			break;

		}
	}
	return t;
}


static int check_timer(void)
{
	struct timespec ts;

	if (clock_getres(CLOCK_MONOTONIC, &ts))
		return 1;

	return (ts.tv_sec != 0 || ts.tv_nsec != 1);
}

/*
 * Finds the tracing directory in a mounted debugfs
 */
char *get_debugfileprefix(void)
{
	char type[100];
	FILE *fp;
	int size;
	int found = 0;
	struct stat s;

	if (debugfileprefix[0] != '\0')
		goto out;

	/* look in the "standard" mount point first */
	if ((stat("/sys/kernel/debug/tracing", &s) == 0) && S_ISDIR(s.st_mode)) {
		strcpy(debugfileprefix, "/sys/kernel/debug/tracing/");
		goto out;
	}

	/* now look in the "other standard" place */
	if ((stat("/debug/tracing", &s) == 0) && S_ISDIR(s.st_mode)) {
		strcpy(debugfileprefix, "/debug/tracing/");
		goto out;
	}

	/* oh well, parse /proc/mounts and see if it's there */
	if ((fp = fopen("/proc/mounts", "r")) == NULL)
		goto out;

	while (fscanf(fp, "%*s %"
		      STR(MAX_PATH)
		      "s %99s %*s %*d %*d\n",
		      debugfileprefix, type) == 2) {
		if (strcmp(type, "debugfs") == 0) {
			found = 1;
			break;
		}
		/* stupid check for systemd-style autofs mount */
		if ((strcmp(debugfileprefix, "/sys/kernel/debug") == 0) &&
		    (strcmp(type, "systemd") == 0)) {
			found = 1;
			break;
		}
	}
	fclose(fp);

	if (!found) {
		debugfileprefix[0] = '\0';
		goto out;
	}

	size = sizeof(debugfileprefix) - strlen(debugfileprefix);
	strncat(debugfileprefix, "/tracing/", size);

out:
	return debugfileprefix;
}

static void set_latency_target(void)
{
	struct stat s;
	int err;

	errno = 0;
	err = stat("/dev/cpu_dma_latency", &s);
	if (err == -1) {
		err_msg_n(errno, "WARN: stat /dev/cpu_dma_latency failed");
		return;
	}

	errno = 0;
	latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
	if (latency_target_fd == -1) {
		err_msg_n(errno, "WARN: open /dev/cpu_dma_latency");
		return;
	}

	errno = 0;
	err = write(latency_target_fd, &latency_target_value, 4);
	if (err < 1) {
		err_msg_n(errno, "# error setting cpu_dma_latency to %d!", latency_target_value);
		close(latency_target_fd);
		return;
	}
	printf("# /dev/cpu_dma_latency set to %dus\n", latency_target_value);
}

static int trace_file_exists(char *name)
{
	struct stat sbuf;
	char *tracing_prefix = get_debugfileprefix();
	char path[MAX_PATH];
	strcat(strcpy(path, tracing_prefix), name);
	return stat(path, &sbuf) ? 0 : 1;
}



int mount_debugfs(char *path)
{
	char *mountpoint = path;
	char cmd[MAX_PATH];
	char *prefix;
	int ret;

	/* if it's already mounted just return */
	prefix = get_debugfileprefix();
	if (strlen(prefix) != 0) {
		info(1, "debugfs mountpoint: %s\n", prefix);
		return 0;
	}
	if (!mountpoint)
		mountpoint = "/sys/kernel/debug";

	sprintf(cmd, "mount -t debugfs debugfs %s", mountpoint);
	ret = system(cmd);
	if (ret != 0) {
		fprintf(stderr, "Error mounting debugfs at %s: %s\n",
			mountpoint, strerror(errno));
		return -1;
	}
	return 0;
}

static void debugfs_prepare(void)
{
	if (mount_debugfs(NULL))
		fatal("could not mount debugfs");

	fileprefix = get_debugfileprefix();
	if (!trace_file_exists("tracing_enabled") &&
	    !trace_file_exists("tracing_on"))
		warn("tracing_enabled or tracing_on not found\n"
		     "debug fs not mounted");
}

static void open_tracemark_fd(void)
{
	char path[MAX_PATH];

	/*
	 * open the tracemark file if it's not already open
	 */
	if (tracemark_fd < 0) {
		sprintf(path, "%s/%s", fileprefix, "trace_marker");
		tracemark_fd = open(path, O_WRONLY);
		if (tracemark_fd < 0) {
			warn("unable to open trace_marker file: %s\n", path);
			return;
		}
	}

	/*
	 * if we're not tracing and the tracing_on fd is not open,
	 * open the tracing_on file so that we can stop the trace
	 * if we hit a breaktrace threshold
	 */
	if (trace_fd < 0) {
		sprintf(path, "%s/%s", fileprefix, "tracing_on");
		if ((trace_fd = open(path, O_WRONLY)) < 0)
			warn("unable to open tracing_on file: %s\n", path);
	}
}


static void close_tracemark_fd(void)
{
	if (tracemark_fd > 0)
		close(tracemark_fd);

	if (trace_fd > 0)
		close(trace_fd);
}

void disable_trace_mark(void)
{
	close_tracemark_fd();
}

void enable_trace_mark(void)
{
	debugfs_prepare();
	open_tracemark_fd();
}


static void get_timestamp(char *tsbuf)
{
	struct timeval tv;
	struct tm *tm;
	time_t t;

	gettimeofday(&tv, NULL);
	t = tv.tv_sec;
	tm = localtime(&t);
	/* RFC 2822-compliant date format */
	strftime(tsbuf, MAX_TS_SIZE, "%a, %d %b %Y %T %z", tm);
}

void rt_init(int argc, char *argv[])
{
	int offset = 0;
	int len, i;

	test_cmdline[0] = '\0';

	/*
	 * getopt_long() permutes the contents of argv as it scans, so
	 * that eventually all the nonoptions are at the end. Make a
	 * copy before calling getopt_long().
	 */
	for (i = 0; i < argc;) {
		len = strlen(argv[i]);
		if (offset + len + 1 >= MAX_COMMAND_LINE)
			break;

		strcat(test_cmdline, argv[i]);
		i++;
		if (i < argc)
			strcat(test_cmdline, " ");

		offset += len + 1;
	}

	get_timestamp(ts_start);
}

void rt_write_json(const char *filename, int return_code,
		  void (*cb)(FILE *, void *),
		  void *data)
{
	unsigned char buf[1];
	struct utsname uts;
	char ts_end[MAX_TS_SIZE];
	FILE *f, *s;
	size_t n;
	int rt = 0;

	if (!filename || !strcmp("-", filename)) {
		f = stdout;
	} else {
		f = fopen(filename, "w");
		if (!f)
			err_exit(errno, "Failed to open '%s'\n", filename);
	}

	get_timestamp(ts_end);

	s = fopen("/sys/kernel/realtime", "r");
	if (s) {
		n = fread(buf, 1, 1, s);
		if (n == 1 && buf[0] == '1')
			rt = 1;
		fclose(s);
	}

	if (uname(&uts))
		err_exit(errno, "Could not retrieve system information");

	fprintf(f, "{\n");
	fprintf(f, "  \"file_version\": 1,\n");
	fprintf(f, "  \"cmdline:\": \"%s\",\n", test_cmdline);
	fprintf(f, "  \"rt_test_version:\": \"%1.2f\",\n", "KEYSTONE CUSTOM");
	fprintf(f, "  \"start_time\": \"%s\",\n", ts_start);
	fprintf(f, "  \"end_time\": \"%s\",\n", ts_end);
	fprintf(f, "  \"return_code\": %d,\n", return_code);
	fprintf(f, "  \"sysinfo\": {\n");
	fprintf(f, "    \"sysname\": \"%s\",\n", uts.sysname);
	fprintf(f, "    \"nodename\": \"%s\",\n", uts.nodename);
	fprintf(f, "    \"release\": \"%s\",\n", uts.release);
	fprintf(f, "    \"version\": \"%s\",\n", uts.version);
	fprintf(f, "    \"machine\": \"%s\",\n", uts.machine);
	fprintf(f, "    \"realtime\": %d\n", rt);

	if (cb) {
		fprintf(f, "  },\n");
		(cb)(f, data);
	} else {
		fprintf(f, "  }\n");
	}

	fprintf(f, "}\n");

	if (!filename || strcmp("-", filename))
		fclose(f);
}



static void print_tids(struct thread_param *par[], int nthreads)
{
	int i;

	printf("# Thread Ids:");
	for (i = 0; i < nthreads; i++)
		printf(" %05d", par[i]->stats->tid);
	printf("\n");
}


int numa_initialize(void)
{
	static int is_initialized;	// Only call numa_available once
	static int numa;

	if (is_initialized == 1)
		return numa;

	if (numa_available() != -1)
		numa = 1;

	is_initialized = 1;

	return numa;
}

static void *
threadalloc(size_t size, int node)
{
	if (node == -1)
		return malloc(size);
	return numa_alloc_onnode(size, node);
}

static inline void rt_bitmask_free(struct bitmask *mask)
{
	numa_bitmask_free(mask);
}

/*
 * Use new bit mask CPU affinity behavior
 */
static int rt_numa_numa_node_of_cpu(int cpu)
{
	int node;
	node = numa_node_of_cpu(cpu);
	if (node == -1)
		fatal("invalid cpu passed to numa_node_of_cpu(%d)\n", cpu);
	return node;
}

int get_available_cpus(struct bitmask *cpumask)
{
	cpu_set_t cpuset;
	int ret;

	if (cpumask)
		return numa_bitmask_weight(cpumask);

	CPU_ZERO(&cpuset);

	ret = sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	if (ret < 0)
		fatal("sched_getaffinity failed: %m\n");

	return CPU_COUNT(&cpuset);
}

int cpu_for_thread_sp(int thread_num, int max_cpus, struct bitmask *cpumask)
{
	int m, cpu, i, num_cpus;

	num_cpus = numa_bitmask_weight(cpumask);

	if (num_cpus == 0)
		fatal("No allowable cpus to run on\n");

	m = thread_num % num_cpus;

	/* there are num_cpus bits set, we want position of m'th one */
	for (i = 0, cpu = 0; i < max_cpus; i++) {
		if (numa_bitmask_isbitset(cpumask, i)) {
			if (cpu == m)
				return i;
			cpu++;
		}
	}
	warn("Bug in cpu mask handling code.\n");
	return 0;
}

/* cpu_for_thread AFFINITY_USECURRENT */
int cpu_for_thread_ua(int thread_num, int max_cpus)
{
	int res, num_cpus, i, m, cpu;
	cpu_set_t cpuset;

	CPU_ZERO(&cpuset);

	res = sched_getaffinity(0, sizeof(cpu_set_t), &cpuset);
	if (res != 0)
		fatal("sched_getaffinity failed: %s\n", strerror(res));

	num_cpus = CPU_COUNT(&cpuset);
	m = thread_num % num_cpus;

	for (i = 0, cpu = 0; i < max_cpus; i++) {
		if (CPU_ISSET(i, &cpuset)) {
			if (cpu == m)
				return i;
			cpu++;
		}
	}

	warn("Bug in cpu mask handling code.\n");
	return 0;
}

static void
threadfree(void *ptr, size_t size, int node)
{
	if (node == -1)
		free(ptr);
	else
		numa_free(ptr, size);
}

int check_privs(void)
{
	int policy = sched_getscheduler(0);
	struct sched_param param, old_param;

	/* if we're already running a realtime scheduler
	 * then we *should* be able to change things later
	 */
	if (policy == SCHED_FIFO || policy == SCHED_RR)
		return 0;

	/* first get the current parameters */
	if (sched_getparam(0, &old_param)) {
		fprintf(stderr, "unable to get scheduler parameters\n");
		return 1;
	}
	param = old_param;

	/* try to change to SCHED_FIFO */
	param.sched_priority = 1;
	if (sched_setscheduler(0, SCHED_FIFO, &param)) {
		fprintf(stderr, "Unable to change scheduling policy!\n");
		fprintf(stderr, "either run as root or join realtime group\n");
		return 1;
	}

	/* we're good; change back and return success */
	return sched_setscheduler(0, policy, &old_param);
}




static void set_main_thread_affinity(struct bitmask *cpumask)
{
	int res;

	errno = 0;
	res = numa_sched_setaffinity(getpid(), cpumask);
	if (res != 0)
		warn("Couldn't setaffinity in main thread: %s\n",
		     strerror(errno));
}

/* Running status shared memory open */
static int rstat_shm_open(void)
{
	int fd;
	pid_t pid;

	pid = getpid();

	snprintf(shm_name, SHM_BUF_SIZE, "%s%d", "/cyclictest", pid);

	errno = 0;
	fd = shm_unlink(shm_name);

	if ((fd == -1) && (errno != ENOENT)) {
		fprintf(stderr, "ERROR: shm_unlink %s\n", strerror(errno));
		return fd;
	}

	errno = 9;
	fd = shm_open(shm_name, O_RDWR|O_CREAT|O_EXCL, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
	if (fd == -1)
		fprintf(stderr, "ERROR: shm_open %s\n", strerror(errno));

	rstat_fd = fd;

	return fd;
}

static int rstat_ftruncate(int fd, off_t len)
{
	int err;

	errno = 0;
	err = ftruncate(fd, len);
	if (err)
		fprintf(stderr, "ftruncate error %s\n", strerror(errno));

	return err;
}

static void *rstat_mmap(int fd)
{
	void *mptr;

	errno = 0;
	mptr = mmap(0, _SC_PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);

	if (mptr == (void*)-1)
		fprintf(stderr, "ERROR: mmap, %s\n", strerror(errno));

	return mptr;
}

static int rstat_mlock(void *mptr)
{
	int err;

	errno = 0;
	err = mlock(mptr, _SC_PAGE_SIZE);
	if (err == -1)
		fprintf(stderr, "ERROR, mlock %s\n", strerror(errno));

	return err;
}

static void rstat_setup(void)
{
	int res;
	void *mptr = NULL;

	int sfd = rstat_shm_open();
	if (sfd < 0)
		goto rstat_err;

	res = rstat_ftruncate(sfd, _SC_PAGE_SIZE);
	if (res)
		goto rstat_err1;

	mptr = rstat_mmap(sfd);
	if (mptr == MAP_FAILED)
		goto rstat_err1;

	res = rstat_mlock(mptr);
	if (res)
		goto rstat_err2;

	return;

rstat_err2:
	munmap(mptr, _SC_PAGE_SIZE);
rstat_err1:
	close(sfd);
	shm_unlink(shm_name);
rstat_err:
	rstat_fd = -1;
	return;
}

void hist_print_oflows(struct histogram *h, FILE *f)
{
	unsigned long i;

	for (i = 0; i < h->oflow_count; i++) {
		if (i >= h->oflow_bufsize)
			break;
		if (i != 0)
			fprintf(f, " ");
		fprintf(f, "%05lu", h->oflows[i]);
	}

	if (i >= h->oflow_bufsize)
		fprintf(f, " # %05lu others", h->oflow_count - h->oflow_bufsize);
}


static void print_hist(struct thread_param *par[], int nthreads)
{
	int i, j;
	unsigned long maxmax, alloverflows;
	FILE *fd;

	if (use_histfile) {
		fd = fopen(histfile, "w");
		if (!fd) {
			perror("opening histogram file:");
			return;
		}
	} else {
		fd = stdout;
	}

	fprintf(fd, "# Histogram\n");
	for (i = 0; i < histogram; i++) {
		unsigned long flags = 0;
		char buf[64];

		snprintf(buf, sizeof(buf), "%06d ", i);

		if (histofall)
			flags |= HSET_PRINT_SUM;
		hset_print_bucket(&hset, fd, buf, i, flags);
	}
	fprintf(fd, "# Min Latencies:");
	for (j = 0; j < nthreads; j++)
		fprintf(fd, " %05lu", par[j]->stats->min);
	fprintf(fd, "\n");
	fprintf(fd, "# Avg Latencies:");
	for (j = 0; j < nthreads; j++)
		fprintf(fd, " %05lu", par[j]->stats->cycles ?
		       (long)(par[j]->stats->avg/par[j]->stats->cycles) : 0);
	fprintf(fd, "\n");
	fprintf(fd, "# Max Latencies:");
	maxmax = 0;
	for (j = 0; j < nthreads; j++) {
		fprintf(fd, " %05lu", par[j]->stats->max);
		if (par[j]->stats->max > maxmax)
			maxmax = par[j]->stats->max;
	}
	if (histofall && nthreads > 1)
		fprintf(fd, " %05lu", maxmax);
	fprintf(fd, "\n");
	fprintf(fd, "# Histogram Overflows:");
	alloverflows = 0;
	for (j = 0; j < nthreads; j++) {
		fprintf(fd, " %05lu", par[j]->stats->hist->oflow_count);
		alloverflows += par[j]->stats->hist->oflow_count;
	}
	if (histofall && nthreads > 1)
		fprintf(fd, " %05lu", alloverflows);
	fprintf(fd, "\n");

	fprintf(fd, "# Histogram Overflow at cycle number:\n");
	for (i = 0; i < nthreads; i++) {
		fprintf(fd, "# Thread %d: ", i);
		hist_print_oflows(par[i]->stats->hist, fd);
		fprintf(fd, "\n");
	}
	if (smi) {
		fprintf(fd, "# SMIs:");
		for (i = 0; i < nthreads; i++)
			fprintf(fd, " %05lu", par[i]->stats->smi_count);
		fprintf(fd, "\n");
	}

	fprintf(fd, "\n");

	if (use_histfile)
		fclose(fd);
}
static void print_stat(FILE *fp, struct thread_param *par, int index, int verbose, int quiet)
{
	struct thread_stat *stat = par->stats;

	if (!verbose) {
		
		char *fmt;
		if (use_nsecs)
			fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%8ld Avg:%8ld Max:%8ld";
		else
			fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%5ld Avg:%5ld Max:%8ld";

		fprintf(fp, fmt, index, stat->tid, par->prio,
			par->interval, stat->cycles, stat->min,
			stat->act, stat->cycles ?
			(long)(stat->avg/stat->cycles) : 0, stat->max);

		fprintf(fp, "\n");
	
	} else {
		while (stat->cycles != stat->cyclesread) {
			long diff = stat->values
			    [stat->cyclesread & par->bufmsk];

			if (diff > stat->redmax) {
				stat->redmax = diff;
				stat->cycleofmax = stat->cyclesread;
			}
			if (++stat->reduce == oscope_reduction) {
				fprintf(fp, "%8d:%8lu:%8ld\n", index,
					stat->cycleofmax, stat->redmax);

				stat->reduce = 0;
				stat->redmax = 0;
			}
			stat->cyclesread++;
		}
	}
}

static void rstat_print_stat(struct thread_param *par, int index, int verbose, int quiet)
{
	struct thread_stat *stat = par->stats;
	int fd = rstat_fd;

	if (!verbose) {
		char *fmt;
		if (use_nsecs)
			fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%8ld Avg:%8ld Max:%8ld";
		else
			fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%5ld Avg:%5ld Max:%8ld";

		dprintf(fd, fmt, index, stat->tid, par->prio,
			par->interval, stat->cycles, stat->min,
			stat->act, stat->cycles ?
			(long)(stat->avg/stat->cycles) : 0, stat->max);

		dprintf(fd, "\n");
	} else {
		while (stat->cycles != stat->cyclesread) {
			unsigned long diff_smi;
			long diff = stat->values
			    [stat->cyclesread & par->bufmsk];

			if (diff > stat->redmax) {
				stat->redmax = diff;
				stat->cycleofmax = stat->cyclesread;
			}
			if (++stat->reduce == oscope_reduction) {
				dprintf(fd, "%8d:%8lu:%8ld%8ld\n",
					index, stat->cycleofmax,
					stat->redmax, diff_smi);

				stat->reduce = 0;
				stat->redmax = 0;
			}
			stat->cyclesread++;
		}
	}
}


/* Print usage information */
static void display_help(int error)
{
	printf("cyclictest + enclave");
	printf("Usage:\n"
	       "cyclictest <options>\n\n"
	       "-a [CPUSET] --affinity     Run thread #N on processor #N, if possible,\n"
	       "                           or if CPUSET given, pin threads to that set of\n"
	       "                           processors in round-robin order.\n"
	       "                           E.g. -a 2 pins all threads to CPU 2, but\n"
	       "                           -a 3-5,0 -t 5 will run the first and fifth threads\n"
	       "                           on CPU 0, the second thread on CPU 3,\n"
	       "                           the third thread on CPU 4,\n"
	       "                           and the fourth thread on CPU 5.\n"
	       "-A USEC  --aligned=USEC    align thread wakeups to a specific offset\n"
	       "-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
	       "-c CLOCK --clock=CLOCK     select clock\n"
	       "                           0 = CLOCK_MONOTONIC (default)\n"
	       "                           1 = CLOCK_REALTIME\n"
	       "         --deepest-idle-state=n\n"
	       "                           Reduce exit from idle latency by limiting idle state\n"
	       "                           up to n on used cpus (-1 disables all idle states).\n"
	       "                           Power management is not suppresed on other cpus.\n"
	       "         --delay N          Delay N seconds before starting the test\n"
	       "                           Uses high-priority nanosleep to wake up\n"
	       "                           even under high load conditions\n"
	       "         --default-system  Don't attempt to tune the system from cyclictest.\n"
	       "                           Power management is not suppressed.\n"
	       "                           This might give poorer results, but will allow you\n"
	       "                           to discover if you need to tune the system\n"
	       "-d DIST  --distance=DIST   distance of thread intervals in us, default=500\n"
	       "-D       --duration=TIME   specify a length for the test run.\n"
	       "                           Append 'm', 'h', or 'd' to specify minutes, hours or days.\n"
	       "-F       --fifo=<path>     create a named pipe at path and write stats to it\n"
	       "-h       --histogram=US    dump a latency histogram to stdout after the run\n"
	       "                           US is the max latency time to be tracked in microseconds\n"
	       "			   This option runs all threads at the same priority.\n"
	       "-H       --histofall=US    same as -h except with an additional summary column\n"
	       "	 --histfile=<path> dump the latency histogram to <path> instead of stdout\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "         --json=FILENAME   write final results into FILENAME, JSON formatted\n"
	       "	 --laptop	   Save battery when running cyclictest\n"
	       "			   This will give you poorer realtime results\n"
	       "			   but will not drain your battery so quickly\n"
	       "         --latency=PM_QOS  power management latency target value\n"
	       "                           This value is written to /dev/cpu_dma_latency\n"
	       "                           and affects c-states. The default is 0\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "         --mainaffinity=CPUSET\n"
	       "			   Run the main thread on CPU #N. This only affects\n"
	       "                           the main thread and not the measurement threads\n"
	       "-m       --mlockall        lock current and future memory allocations\n"
	       "-M       --refresh_on_max  delay updating the screen until a new max\n"
	       "			   latency is hit. Useful for low bandwidth.\n"
		   "-n		 --no_enclave	   don't run enclave (debugging)\n"
	       "-N       --nsecs           print results in ns instead of us (default us)\n"
	       "-o RED   --oscope=RED      oscilloscope mode, reduce verbose output by RED\n"
	       "-p PRIO  --priority=PRIO   priority of highest prio thread\n"
	       "	 --policy=NAME     policy of measurement thread, where NAME may be one\n"
	       "                           of: other, normal, batch, idle, fifo or rr.\n"
	       "	 --priospread      spread priority levels starting at specified value\n"
	       "-q       --quiet           print a summary only on exit\n"
	       "-r       --relative        use relative timer instead of absolute\n"
	       "-R       --resolution      check clock resolution, calling clock_gettime() many\n"
	       "                           times.  List of clock_gettime() values will be\n"
	       "                           reported with -X\n"
	       "         --secaligned [USEC] align thread wakeups to the next full second\n"
	       "                           and apply the optional offset\n"
	       "-s       --system          use sys_nanosleep and sys_setitimer\n"
	       "-S       --smp             Standard SMP testing: options -a -t and same priority\n"
	       "                           of all threads\n"
	       "	--spike=<trigger>  record all spikes > trigger\n"
	       "	--spike-nodes=[num of nodes]\n"
	       "			   These are the maximum number of spikes we can record.\n"
	       "			   The default is 1024 if not specified\n"
#ifdef ARCH_HAS_SMI_COUNTER
               "         --smi             Enable SMI counting\n"
#endif
	       "-t       --threads         one thread per available processor\n"
	       "-t [NUM] --threads=NUM     number of threads:\n"
	       "                           without NUM, threads = max_cpus\n"
	       "                           without -t default = 1\n"
	       "         --tracemark       write a trace mark when -b latency is exceeded\n"
	       "-u       --unbuffered      force unbuffered output for live processing\n"
	       "-v       --verbose         output values on stdout for statistics\n"
	       "                           format: n:c:v n=tasknum c=count v=value in us\n"
	       "	 --dbg_cyclictest  print info useful for debugging cyclictest\n"
	       "-x	 --posix_timers    use POSIX timers instead of clock_nanosleep.\n"
		);
	if (error)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}

enum {
	AFFINITY_UNSPECIFIED,
	AFFINITY_SPECIFIED,
	AFFINITY_USECURRENT
};

static int use_nanosleep = MODE_CLOCK_NANOSLEEP;
static int timermode = TIMER_ABSTIME;
static int use_system;
static int priority;
static int policy = SCHED_OTHER;	/* default policy if not specified */
static int num_threads = 1;
static int max_cycles;
static int clocksel = 0;
static int quiet;
static int interval = DEFAULT_INTERVAL;
static int distance = -1;
static struct bitmask *affinity_mask = NULL;
static struct bitmask *main_affinity_mask = NULL;
static int smp = 0;
static int setaffinity = AFFINITY_UNSPECIFIED;

static int clocksources[] = {
	CLOCK_MONOTONIC,
	CLOCK_REALTIME,
};

static void handlepolicy(char *polname)
{
	if (strncasecmp(polname, "other", 5) == 0)
		policy = SCHED_OTHER;
	else if (strncasecmp(polname, "batch", 5) == 0)
		policy = SCHED_BATCH;
	else if (strncasecmp(polname, "idle", 4) == 0)
		policy = SCHED_IDLE;
	else if (strncasecmp(polname, "fifo", 4) == 0)
		policy = SCHED_FIFO;
	else if (strncasecmp(polname, "rr", 2) == 0)
		policy = SCHED_RR;
	else	/* default policy if we don't recognize the request */
		policy = SCHED_OTHER;
}

static char *policyname(int policy)
{
	char *policystr = "";

	switch(policy) {
	case SCHED_OTHER:
		policystr = "other";
		break;
	case SCHED_FIFO:
		policystr = "fifo";
		break;
	case SCHED_RR:
		policystr = "rr";
		break;
	case SCHED_BATCH:
		policystr = "batch";
		break;
	case SCHED_IDLE:
		policystr = "idle";
		break;
	}
	return policystr;
}

static void write_stats(FILE *f, void *data __attribute__ ((unused)))
{
	struct thread_param **par = parameters;
	int i;
	struct thread_stat *s;

	fprintf(f, "  \"num_threads\": %d,\n", num_threads);
	fprintf(f, "  \"resolution_in_ns\": %u,\n", use_nsecs);
	fprintf(f, "  \"thread\": {\n");
	for (i = 0; i < num_threads; i++) {
		s = par[i]->stats;
		fprintf(f, "    \"%u\": {\n", i);
		if (s->hist) {
			fprintf(f, "      \"histogram\": {");
			hist_print_json(s->hist, f);
			fprintf(f, "      },\n");
		}
		fprintf(f, "      \"cycles\": %ld,\n", s->cycles);
		fprintf(f, "      \"min\": %ld,\n", s->min);
		fprintf(f, "      \"max\": %ld,\n", s->max);
		fprintf(f, "      \"avg\": %.2f,\n", s->avg/s->cycles);
		fprintf(f, "      \"cpu\": %d,\n", par[i]->cpu);
		fprintf(f, "      \"node\": %d\n", par[i]->node);
		fprintf(f, "    }%s\n", i == num_threads - 1 ? "" : ",");
	}
	fprintf(f, "  }\n");
}

enum option_values {
	OPT_AFFINITY=1, OPT_BREAKTRACE, OPT_CLOCK,
	OPT_DEFAULT_SYSTEM, OPT_DISTANCE, OPT_DURATION, OPT_LATENCY,
	OPT_FIFO, OPT_HISTOGRAM, OPT_HISTOFALL, OPT_HISTFILE,
	OPT_INTERVAL, OPT_JSON, OPT_MAINAFFINITY, OPT_LOOPS, OPT_MLOCKALL,
	OPT_REFRESH, OPT_NANOSLEEP, OPT_NSECS, OPT_OSCOPE, OPT_PRIORITY,
	OPT_QUIET, OPT_PRIOSPREAD, OPT_RELATIVE, OPT_RESOLUTION,
	OPT_SYSTEM, OPT_SMP, OPT_THREADS, OPT_TRIGGER,
	OPT_TRIGGER_NODES, OPT_UNBUFFERED, OPT_NUMA, OPT_VERBOSE,
	OPT_DBGCYCLIC, OPT_POLICY, OPT_HELP, OPT_NUMOPTS,
	OPT_ALIGNED, OPT_SECALIGNED, OPT_LAPTOP, OPT_SMI,
	OPT_TRACEMARK, OPT_POSIX_TIMERS, OPT_DEEPEST_IDLE_STATE, OPT_NO_ENCLAVE,
	OPT_DELAY
};


/* Process commandline options */
static void process_options(int argc, char *argv[], int max_cpus)
{
	int error = 0;
	int option_affinity = 0;

	for (;;) {
		int option_index = 0;
			
		/*
		 * Options for getopt
		 * Ordered alphabetically by single letter name
		 */
		static struct option long_options[] = {
			{"affinity",         optional_argument, NULL, OPT_AFFINITY},
			{"aligned",          optional_argument, NULL, OPT_ALIGNED },
			{"breaktrace",       required_argument, NULL, OPT_BREAKTRACE },
			{"clock",            required_argument, NULL, OPT_CLOCK },
			{"default-system",   no_argument,       NULL, OPT_DEFAULT_SYSTEM },
			{"distance",         required_argument, NULL, OPT_DISTANCE },
			{"duration",         required_argument, NULL, OPT_DURATION },
			{"latency",          required_argument, NULL, OPT_LATENCY },
			{"fifo",             required_argument, NULL, OPT_FIFO },
			{"histogram",        required_argument, NULL, OPT_HISTOGRAM },
			{"histofall",        required_argument, NULL, OPT_HISTOFALL },
			{"histfile",	     required_argument, NULL, OPT_HISTFILE },
			{"interval",         required_argument, NULL, OPT_INTERVAL },
			{"json",             required_argument, NULL, OPT_JSON },
			{"laptop",	     no_argument,	NULL, OPT_LAPTOP },
			{"loops",            required_argument, NULL, OPT_LOOPS },
			{"mainaffinity",     required_argument, NULL, OPT_MAINAFFINITY},
			{"mlockall",         no_argument,       NULL, OPT_MLOCKALL },
			{"no_enclave",       no_argument,       NULL, OPT_NO_ENCLAVE },
			{"refresh_on_max",   no_argument,       NULL, OPT_REFRESH },
			{"nsecs",            no_argument,       NULL, OPT_NSECS },
			{"oscope",           required_argument, NULL, OPT_OSCOPE },
			{"priority",         required_argument, NULL, OPT_PRIORITY },
			{"quiet",            no_argument,       NULL, OPT_QUIET },
			{"priospread",       no_argument,       NULL, OPT_PRIOSPREAD },
			{"relative",         no_argument,       NULL, OPT_RELATIVE },
			{"resolution",       no_argument,       NULL, OPT_RESOLUTION },
			{"secaligned",       optional_argument, NULL, OPT_SECALIGNED },
			{"system",           no_argument,       NULL, OPT_SYSTEM },
			{"smi",              no_argument,       NULL, OPT_SMI },
			{"smp",              no_argument,       NULL, OPT_SMP },
			{"spike",	     required_argument, NULL, OPT_TRIGGER },
			{"spike-nodes",	     required_argument, NULL, OPT_TRIGGER_NODES },
			{"threads",          optional_argument, NULL, OPT_THREADS },
			{"tracemark",	     no_argument,	NULL, OPT_TRACEMARK },
			{"unbuffered",       no_argument,       NULL, OPT_UNBUFFERED },
			{"verbose",          no_argument,       NULL, OPT_VERBOSE },
			{"dbg_cyclictest",   no_argument,       NULL, OPT_DBGCYCLIC },
			{"policy",           required_argument, NULL, OPT_POLICY },
			{"help",             no_argument,       NULL, OPT_HELP },
			{"posix_timers",     no_argument,	NULL, OPT_POSIX_TIMERS },
			{"deepest-idle-state", required_argument,	NULL, OPT_DEEPEST_IDLE_STATE },
			{"delay",            required_argument, NULL, OPT_DELAY },
			{NULL, 0, NULL, 0 },
		};
		int c = getopt_long(argc, argv, "a::A::b:c:d:D:F:h:H:i:l:MNo:p:mnqrRsSt::uvD:x",
				    long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
		case OPT_AFFINITY:
			option_affinity = 1;
			/* smp sets AFFINITY_USECURRENT in OPT_SMP */
			if (smp)
				break;
			numa = numa_initialize();
			if (optarg) {
				parse_cpumask(optarg, max_cpus, &affinity_mask);
				setaffinity = AFFINITY_SPECIFIED;
			} else if (optind < argc &&
				   (atoi(argv[optind]) ||
				    argv[optind][0] == '0' ||
				    argv[optind][0] == '!' ||
				    argv[optind][0] == '+' ||
				    argv[optind][0] == 'a')) {
				parse_cpumask(argv[optind], max_cpus, &affinity_mask);
				setaffinity = AFFINITY_SPECIFIED;
			} else {
				setaffinity = AFFINITY_USECURRENT;
			}

			if (setaffinity == AFFINITY_SPECIFIED && !affinity_mask)
				display_help(1);
			if (verbose && affinity_mask)
				printf("Using %u cpus.\n",
					numa_bitmask_weight(affinity_mask));
			break;
		case 'A':
		case OPT_ALIGNED:
			aligned = 1;
			if (optarg != NULL)
				offset = atoi(optarg) * 1000;
			else if (optind < argc && atoi(argv[optind]))
				offset = atoi(argv[optind]) * 1000;
			else
				offset = 0;
			break;
		case 'b':
		case OPT_BREAKTRACE:
			tracelimit = atoi(optarg); break;
		case 'c':
		case OPT_CLOCK:
			clocksel = atoi(optarg); break;
		case OPT_DEFAULT_SYSTEM:
			power_management = 1; break;
		case 'd':
		case OPT_DISTANCE:
			distance = atoi(optarg); break;
		case 'D':
		case OPT_DURATION:
			duration = parse_time_string(optarg); break;
		case 'F':
		case OPT_FIFO:
			use_fifo = 1;
			strncpy(fifopath, optarg, strnlen(optarg, MAX_PATH-1));
			break;
		case 'H':
		case OPT_HISTOFALL:
			histofall = 1; /* fall through */
		case 'h':
		case OPT_HISTOGRAM:
			histogram = atoi(optarg);
			if (!histogram)
				display_help(1);
			break;
		case OPT_HISTFILE:
			use_histfile = 1;
			strncpy(histfile, optarg, strnlen(optarg, MAX_PATH-1));
			break;
		case 'i':
		case OPT_INTERVAL:
			interval = atoi(optarg); break;
		case OPT_JSON:
			strncpy(jsonfile, optarg, strnlen(optarg, MAX_PATH-1));
			break;
		case 'l':
		case OPT_LOOPS:
			max_cycles = atoi(optarg); break;
		case OPT_MAINAFFINITY:
			if (optarg) {
				parse_cpumask(optarg, max_cpus, &main_affinity_mask);
			} else if (optind < argc &&
			           (atoi(argv[optind]) ||
			            argv[optind][0] == '0' ||
			            argv[optind][0] == '!')) {
				parse_cpumask(argv[optind], max_cpus, &main_affinity_mask);
			}
			break;
		case 'm':
		case OPT_MLOCKALL:
			lockall = 1; break;
		case 'M':
		case OPT_REFRESH:
			refresh_on_max = 1; break;
		case 'n':
		case OPT_NO_ENCLAVE:
			no_enclave = 1;
			break;
		case 'N':
		case OPT_NSECS:
			use_nsecs = 1; break;
		case 'o':
		case OPT_OSCOPE:
			oscope_reduction = atoi(optarg); break;
		case 'p':
		case OPT_PRIORITY:
			priority = atoi(optarg);
			if (policy != SCHED_FIFO && policy != SCHED_RR)
				policy = SCHED_FIFO;
			break;
		case 'q':
		case OPT_QUIET:
			quiet = 1; break;
		case 'r':
		case OPT_RELATIVE:
			timermode = TIMER_RELTIME; break;
		case 'R':
		case OPT_RESOLUTION:
			check_clock_resolution = 1; break;
		case OPT_SECALIGNED:
			secaligned = 1;
			if (optarg != NULL)
				offset = atoi(optarg) * 1000;
			else if (optind < argc && atoi(argv[optind]))
				offset = atoi(argv[optind]) * 1000;
			else
				offset = 0;
			break;
		case 's':
		case OPT_SYSTEM:
			use_system = MODE_SYS_OFFSET; break;
		case 'S':
		case OPT_SMP: /* SMP testing */
			if (numa)
				fatal("numa and smp options are mutually exclusive\n");
			smp = 1;
			num_threads = -1; /* update after parsing */
			setaffinity = AFFINITY_USECURRENT;
			break;
		case 't':
		case OPT_THREADS:
			if (smp) {
				warn("-t ignored due to smp mode\n");
				break;
			}
			if (optarg != NULL)
				num_threads = atoi(optarg);
			else if (optind < argc && atoi(argv[optind]))
				num_threads = atoi(argv[optind]);
			else
				num_threads = -1; /* update after parsing */
			break;
		case OPT_TRIGGER:
			trigger = atoi(optarg);
			break;
		case OPT_TRIGGER_NODES:
			if (trigger)
				trigger_list_size = atoi(optarg);
			break;
		case 'u':
		case OPT_UNBUFFERED:
			setvbuf(stdout, NULL, _IONBF, 0); break;
		case 'v':
		case OPT_VERBOSE: verbose = 1; break;
		case 'x':
		case OPT_POSIX_TIMERS:
			use_nanosleep = MODE_CYCLIC; break;
		case '?':
		case OPT_HELP:
			display_help(0); break;

		/* long only options */
		case OPT_PRIOSPREAD:
			priospread = 1; break;
		case OPT_LATENCY:
                          /* power management latency target value */
			  /* note: default is 0 (zero) */
			latency_target_value = atoi(optarg);
			if (latency_target_value < 0)
				latency_target_value = 0;
			break;
		case OPT_POLICY:
			handlepolicy(optarg); break;
		case OPT_DBGCYCLIC:
			ct_debug = 1; break;
		case OPT_LAPTOP:
			laptop = 1; break;
		case OPT_SMI:
#ifdef ARCH_HAS_SMI_COUNTER
			smi = 1;
#else
			fatal("--smi is not available on your arch\n");
#endif
			break;
		case OPT_TRACEMARK:
			trace_marker = 1; break;
		case OPT_DEEPEST_IDLE_STATE:
			deepest_idle_state = atoi(optarg);
			break;
		case OPT_DELAY:
			delay = atoi(optarg);
			break;
		}
	}

	

	if ((use_system == MODE_SYS_OFFSET) && (use_nanosleep == MODE_CYCLIC)) {
		warn("The system option requires clock_nanosleep\n");
		warn("and is not compatible with posix_timers\n");
		warn("Using clock_nanosleep\n");
		use_nanosleep = MODE_CLOCK_NANOSLEEP;
	}
/* if smp wasn't requested, test for numa automatically */
	if (!smp) {
		numa = numa_initialize();
	}

	if (option_affinity) {
		if (smp)
			warn("-a ignored due to smp mode\n");
	}

	if (smi) {
		if (setaffinity == AFFINITY_UNSPECIFIED)
			fatal("SMI counter relies on thread affinity\n");

		if (!has_smi_counter())
			fatal("SMI counter is not supported "
			      "on this processor\n");
	}

	if (clocksel < 0 || clocksel > ARRAY_SIZE(clocksources))
		error = 1;

	if (oscope_reduction < 1)
		error = 1;

	if (oscope_reduction > 1 && !verbose) {
		warn("-o option only meaningful, if verbose\n");
		error = 1;
	}

	if (histogram < 0)
		error = 1;

	if (histogram > HIST_MAX)
		histogram = HIST_MAX;

	if (histogram && distance != -1)
		warn("distance is ignored and set to 0, if histogram enabled\n");
	if (distance == -1)
		distance = DEFAULT_DISTANCE;

	if (priority < 0 || priority > 99)
		error = 1;

	if (num_threads == -1)
		num_threads = get_available_cpus(affinity_mask);

	if (priospread && priority == 0) {
		fprintf(stderr, "defaulting realtime priority to %d\n",
			num_threads+1);
		priority = num_threads+1;
	}

	if (priority && (policy != SCHED_FIFO && policy != SCHED_RR)) {
		fprintf(stderr, "policy and priority don't match: setting policy to SCHED_FIFO\n");
		policy = SCHED_FIFO;
	}

	if ((policy == SCHED_FIFO || policy == SCHED_RR) && priority == 0) {
		fprintf(stderr, "defaulting realtime priority to %d\n",
			num_threads+1);
		priority = num_threads+1;
	}

	if (num_threads < 1)
		error = 1;

	if (aligned && secaligned)
		error = 1;

	if (aligned || secaligned) {
		pthread_barrier_init(&globalt_barr, NULL, num_threads);
		pthread_barrier_init(&align_barr, NULL, num_threads);
	}
	if (error) {
		if (affinity_mask)
			rt_bitmask_free(affinity_mask);
		display_help(1);
	}
}

static void sighand(int sig)
{
	if (sig == SIGUSR1) {
		int i;

		fprintf(stderr, "#---------------------------\n");
		fprintf(stderr, "# cyclictest current status:\n");
		for (i = 0; i < num_threads; i++)
			print_stat(stderr, parameters[i], i, 0, 0);
		fprintf(stderr, "#---------------------------\n");
		return;
	} else if (sig == SIGUSR2) {
		int i;

		if (rstat_fd == -1) {
			fprintf(stderr, "ERROR: rstat_fd not valid\n");
			return;
		}
		rstat_ftruncate(rstat_fd, 0);
		dprintf(rstat_fd, "#---------------------------\n");
		dprintf(rstat_fd, "# cyclictest current status:\n");
		for (i = 0; i < num_threads; i++)
			rstat_print_stat(parameters[i], i, 0, 0);
		dprintf(rstat_fd, "#---------------------------\n");
		return;
	}
	shutdown = 1;
}

/*
 * thread that creates a named fifo and hands out run stats when someone
 * reads from the fifo.
 */
static void *fifothread(void *param __attribute__ ((unused)))
{
	int ret;
	int fd;
	FILE *fp;
	int i;

	unlink(fifopath);
	ret = mkfifo(fifopath, 0666);
	if (ret) {
		fprintf(stderr, "Error creating fifo %s: %s\n", fifopath, strerror(errno));
		return NULL;
	}
	while (!shutdown) {
		fd = open(fifopath, O_WRONLY|O_NONBLOCK);
		if (fd < 0) {
			usleep(500000);
			continue;
		}
		fp = fdopen(fd, "w");
		for (i=0; i < num_threads; i++)
			print_stat(fp, parameters[i], i, 0, 0);
		fclose(fp);
		usleep(250);
	}
	unlink(fifopath);
	return NULL;
}


int
main(int argc, char** argv) {
  
	sigset_t sigset;
	int signum = SIGALRM;
	int mode;
	int cpu;
	int max_cpus = sysconf(_SC_NPROCESSORS_CONF);
	int online_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	int i, ret = -1;
	int status;

	/*remove the initial argv[1->3] that are use for the enclave*/
	int opt_argc = argc - 3;
    char **opt_argv = &argv[3];

	rt_init(argc, argv);
	process_options(opt_argc, opt_argv, max_cpus);

	if (check_privs())
		exit(EXIT_FAILURE);

	if (verbose) {
		printf("Max CPUs = %d\n", max_cpus);
		printf("Online CPUs = %d\n", online_cpus);
	}

	if (affinity_mask != NULL) {
		set_main_thread_affinity(affinity_mask);
		if (verbose)
			printf("Using %u cpus.\n",
				numa_bitmask_weight(affinity_mask));
	}

	if (trigger) {
		int retval;
		retval = trigger_init();
		if (retval != 0) {
			fprintf(stderr, "trigger_init() failed\n");
			exit(EXIT_FAILURE);
		}
	}

	/* lock all memory (prevent swapping) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}

	set_latency_target();

	if (check_timer())
		warn("High resolution timers not available\n");

	if (deepest_idle_state >= -1) {
		if (!have_libcpupower_support()) {
			fprintf(stderr, "cyclictest built without libcpupower, --deepest-idle-state is not supported\n");
			goto out;
		}

		for (i = 0; i < max_cpus; i++) {
			if (affinity_mask && !numa_bitmask_isbitset(affinity_mask, i))
				continue;
			if (save_cpu_idle_disable_state(i) < 0) {
				fprintf(stderr, "Could not save cpu idle state.\n");
				goto out;
			}
			if (set_deepest_cpu_idle_state(i, deepest_idle_state) < 0) {
				fprintf(stderr, "Could not set deepest cpu idle state.\n");
				goto out;
			}
		}
	}

	if (tracelimit && trace_marker)
		enable_trace_mark();

	if (check_timer())
		warn("High resolution timers not available\n");
	
	if (check_clock_resolution) {
		int clock;
		uint64_t diff;
		int k;
		uint64_t min_non_zero_diff = UINT64_MAX;
		struct timespec now;
		struct timespec prev;
		uint64_t reported_resolution = UINT64_MAX;
		struct timespec res;
		struct timespec *time;
		int times;

		clock = clocksources[clocksel];

		if (clock_getres(clock, &res))
			warn("clock_getres failed");
		else
			reported_resolution = (NSEC_PER_SEC * res.tv_sec) + res.tv_nsec;

		times = 1000;

		clock_gettime(clock, &prev);
		for (k=0; k < times; k++)
			clock_gettime(clock, &now);

		
		diff = calcdiff_ns(now, prev);
		if (diff == 0) {
			times = -1;
		} else {
			int call_time;
			call_time = diff / times;         
			times = NSEC_PER_SEC / call_time; 
			times /= 1000;                    
			if (times < 1000)
				times = 1000;
		}

		if ((times <= 0) || (times > 100000))
			times = 100000;

		time = (timespec*)calloc(times, sizeof(*time));

		for (k=0; k < times; k++)
			clock_gettime(clock, &time[k]);

		prev = time[0];
		for (k=1; k < times; k++) {

			diff = calcdiff_ns(time[k], prev);
			prev = time[k];

			if (diff && (diff < min_non_zero_diff))
				min_non_zero_diff = diff;
		}

		free(time);


		if (verbose ||
		    (min_non_zero_diff && (min_non_zero_diff > reported_resolution))) {
			/*
			 * Measured clock resolution includes the time to call
			 * clock_gettime(), so it will be slightly larger than
			 * actual resolution.
			 */
			warn("reported clock resolution: %llu nsec\n",
			     (unsigned long long)reported_resolution);
			warn("measured clock resolution approximately: %llu nsec\n",
			     (unsigned long long)min_non_zero_diff);
		}

	}

	mode = use_nanosleep + use_system;

	sigemptyset(&sigset);
	sigaddset(&sigset, signum);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);
	signal(SIGUSR1, sighand);
	signal(SIGUSR2, sighand);

	/* Set-up shm */
	rstat_setup();

	if (histogram && hset_init(&hset, num_threads, 1, histogram, histogram))
		fatal("failed to allocate histogram of size %d for %d threads\n",
		      histogram, num_threads);

	parameters = (struct thread_param **) calloc(num_threads, sizeof(struct thread_param *));
	if (!parameters)
		goto out;
	statistics = (struct thread_stat **) calloc(num_threads, sizeof(struct thread_stat *));
	if (!statistics)
		goto outpar;
	
	enc_args = (struct enclave_args **) calloc(num_threads, sizeof(struct enclave_args *));
	if (!enc_args)
		goto outpar;

	for (i = 0; i < num_threads; i++) {
		pthread_attr_t attr;
		int node;
		struct thread_param *par;
		struct thread_stat *stat;
		struct enclave_args *enc;

		status = pthread_attr_init(&attr);
		if (status != 0)
			fatal("error from pthread_attr_init for thread %d: %s\n", i, strerror(status));

		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED: cpu = -1; break;
		case AFFINITY_SPECIFIED:
			cpu = cpu_for_thread_sp(i, max_cpus, affinity_mask);
			if (verbose)
				printf("Thread %d using cpu %d.\n", i, cpu);
			break;
		case AFFINITY_USECURRENT:
			cpu = cpu_for_thread_ua(i, max_cpus);
			break;
		default: cpu = -1;
		}

		node = -1;
		if (numa) {
			void *stack;
			void *currstk;
			size_t stksize;
			int node_cpu = cpu;

			if (node_cpu == -1)
				node_cpu = cpu_for_thread_ua(i, max_cpus);

			/* find the memory node associated with the cpu i */
			node = rt_numa_numa_node_of_cpu(node_cpu);

			/* get the stack size set for this thread */
			if (pthread_attr_getstack(&attr, &currstk, &stksize))
				fatal("failed to get stack size for thread %d\n", i);

			/* if the stack size is zero, set a default */
			if (stksize == 0)
				stksize = PTHREAD_STACK_MIN * 2;

			/*  allocate memory for a stack on appropriate node */
			stack = rt_numa_numa_alloc_onnode(stksize, node, node_cpu);

			/* touch the stack pages to pre-fault them in */
			memset(stack, 0, stksize);

			/* set the thread's stack */
			if (pthread_attr_setstack(&attr, stack, stksize))
				fatal("failed to set stack addr for thread %d to 0x%x\n",
				      i, stack+stksize);
		}
		/* allocate the thread's parameter block  */
		parameters[i] = par = (struct thread_param *) threadalloc(sizeof(struct thread_param), node);
		if (par == NULL)
			fatal("error allocating thread_param struct for thread %d\n", i);
		memset(par, 0, sizeof(struct thread_param));

		/* allocate the thread's statistics block */
		statistics[i] = stat = (struct thread_stat *) threadalloc(sizeof(struct thread_stat), node);
		if (stat == NULL)
			fatal("error allocating thread status struct for thread %d\n", i);
		memset(stat, 0, sizeof(struct thread_stat));

		
		/* allocate the thread's enclave agrs block */
		enc_args[i] = enc = (struct enclave_args *) threadalloc(sizeof(struct enclave_args), node);
		if (enc == NULL)
			fatal("error allocating thread enc struct for thread %d\n", i);
		memset(enc, 0, sizeof(struct enclave_args));

		if (histogram)
			stat->hist = &hset.histos[i];

		if (verbose) {
			int bufsize = VALBUF_SIZE * sizeof(long);
			stat->values = (long int *) threadalloc(bufsize, node);
			if (!stat->values)
				goto outall;
			memset(stat->values, 0, bufsize);
			par->bufmsk = VALBUF_SIZE - 1;
			if (smi) {
				stat->smis = (long int *) threadalloc(bufsize, node);
				if (!stat->smis)
					goto outall;
				memset(stat->smis, 0, bufsize);
			}
		}

		par->prio = priority;
		if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
			par->policy = policy;
		else {
			par->policy = SCHED_OTHER;
			force_sched_other = 1;
		}
		if (priospread)
			priority--;
		par->clock = clocksources[clocksel];
		par->mode = mode;
		par->timermode = timermode;
		par->signal = signum;
		par->interval = interval;
		interval += distance;
		if (verbose)
			printf("Thread %d Interval: %d\n", i, interval);

		par->max_cycles = max_cycles;
		par->stats = stat;
		par->enc_args = enc;
		par->node = node;
		par->tnum = i;
		par->cpu = cpu;

		stat->min = 1000000;
		stat->max = 0;
		stat->avg = 0.0;
		stat->threadstarted = 1;
		stat->smi_count = 0;

		enc->eapppath = argv[1];
		enc->runtimepath = argv[2];
		enc->loaderpath = argv[3];

		status = pthread_create(&stat->thread, &attr, enclave_thread, par);
		if (status)
			fatal("failed to create thread %d: %s\n", i, strerror(status));

	}
	/* Restrict the main pid to the affinity specified by the user */
	if (main_affinity_mask != NULL)
		set_main_thread_affinity(main_affinity_mask);

	
	if (use_fifo) {
		status = pthread_create(&fifo_threadid, NULL, fifothread, NULL);
		if (status)
			fatal("failed to create fifo thread: %s\n", strerror(status));
	}


	while (!shutdown) {
		char lavg[256];
		int fd, len, allstopped = 0;
		static char *policystr = NULL;
		static char *slash = NULL;
		static char *policystr2;

		if (!policystr)
			policystr = policyname(policy);

		if (!slash) {
			if (force_sched_other) {
				slash = "/";
				policystr2 = policyname(SCHED_OTHER);
			} else
				slash = policystr2 = "";
		}
		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("policy: %s%s%s: loadavg: %s          \n\n",
			       policystr, slash, policystr2, lavg);
		}

		for (i = 0; i < num_threads; i++) {

			print_stat(stdout, parameters[i], i, verbose, 0);
			if (max_cycles && statistics[i]->cycles >= max_cycles)
				allstopped++;
		}

		usleep(10000);
		if (shutdown || allstopped)
			break;
		if (!verbose && !quiet)
			printf("\033[%dA", num_threads + 2);
		if (refresh_on_max) {
			pthread_mutex_lock(&refresh_on_max_lock);
			if (!shutdown)
				pthread_cond_wait(&refresh_on_max_cond,
						&refresh_on_max_lock);
			pthread_mutex_unlock(&refresh_on_max_lock);
		}

	}
	ret = EXIT_SUCCESS;

 outall:
	shutdown = 1;
	usleep(50000);

	if (!verbose && !quiet && refresh_on_max)
		printf("\033[%dB", num_threads + 2);

	if (strlen(jsonfile) != 0)
		rt_write_json(jsonfile, ret, write_stats, NULL);

	if (quiet)
		quiet = 2;
	for (i = 0; i < num_threads; i++) {
		if (statistics[i]->threadstarted > 0)
			pthread_kill(statistics[i]->thread, SIGTERM);
		if (statistics[i]->threadstarted) {
			pthread_join(statistics[i]->thread, NULL);
			if (quiet && !histogram)
				print_stat(stdout, parameters[i], i, 0, 0);
		}
		if (statistics[i]->values)
			threadfree(statistics[i]->values, VALBUF_SIZE*sizeof(long), parameters[i]->node);
	}

	if (trigger)
		trigger_print();

	if (histogram)
		print_hist(parameters, num_threads);

	if (tracelimit) {
		print_tids(parameters, num_threads);
		if (break_thread_id) {
			printf("# Break thread: %d\n", break_thread_id);
			printf("# Break value: %llu\n", (unsigned long long)break_thread_value);
		}
	}


	for (i=0; i < num_threads; i++) {
		if (!statistics[i])
			continue;
		threadfree(statistics[i], sizeof(struct thread_stat), parameters[i]->node);
	}

 outpar:
	for (i = 0; i < num_threads; i++) {
		if (!parameters[i])
			continue;
		threadfree(parameters[i], sizeof(struct thread_param), parameters[i]->node);
	}
 out:
	/* close any tracer file descriptors */
	disable_trace_mark();

	/* unlock everything */
	if (lockall)
		munlockall();

	/* close the latency_target_fd if it's open */
	if (latency_target_fd >= 0)
		close(latency_target_fd);

	/* restore and free cpu idle disable states */
	if (deepest_idle_state >= -1) {
		for (i = 0; i < max_cpus; i++) {
			if (affinity_mask && !numa_bitmask_isbitset(affinity_mask, i))
				continue;
			restore_cpu_idle_disable_state(i);
		}
	}
	free_cpu_idle_disable_states();

	if (affinity_mask)
		rt_bitmask_free(affinity_mask);

	/* Remove running status shared memory file if it exists */
	if (rstat_fd >= 0)
		shm_unlink(shm_name);

	hset_destroy(&hset);
	exit(ret);


}
