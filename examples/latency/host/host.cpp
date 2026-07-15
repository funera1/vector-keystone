#include "edge/edge_call.h"
#include "host/keystone.h"
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>


#include <sys/time.h>

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

struct enclave_args {
	const char* eapppath;
	const char* runtimepath;
	const char* loaderpath;
};

static struct thread_param **parameters;

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

int
main(int argc, char** argv) {

	Params enc_params;
	enc_params.setFreeMemSize(256 * 1024);
	enc_params.setUntrustedSize(256 * 1024);

	

	for (int i = 0; i < 10; i++){

		struct timespec before_rd, encl_rd = { 0 };

		uint64_t latency;
		uintptr_t rdtime;

		get_rdtime(&before_rd);

		
		Enclave enclave;
		enclave.init(argv[1], argv[2], argv[3], enc_params);

		enclave.registerOcallDispatch(incoming_call_dispatch);
		edge_call_init_internals(
			(uintptr_t)enclave.getSharedBuffer(), enclave.getSharedBufferSize());
		enclave.run(&rdtime);
		rdtimespec((unsigned long) rdtime, &encl_rd);

		latency = calcdiff(encl_rd, before_rd);

		printf("\t%.0f\n", (double) latency);

	}

}
