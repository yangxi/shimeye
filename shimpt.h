#ifndef _GNU_SOURCE
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#endif

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <err.h>
#include <errno.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include <perfmon/pfmlib_perf_event.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <assert.h>
#include <pthread.h>
#include <math.h>

#define MAX_HW_EVENTS (20)
#define SHIM_PAGESIZE (4096)
#define SHIM_BUFFERSIZE (32*1024*1024)

#define DEBUG 1

struct shim_hardware_event {
  int index;
  int fd;
  struct perf_event_attr perf_attr;
  struct perf_event_mmap_page *buf;
  char * name;
};

typedef struct shim_worker_struct shim;

struct shim_worker_struct{
  int cpuid;
  int targetcpu;
  int nr_hw_events;
  struct shim_hardware_event hw_events[MAX_HW_EVENTS];
  int pmc_index[MAX_HW_EVENTS];
  unsigned long *ppid_source;
  int *syscall_source;
  //  int (*probe_other_events)(uint64_t *buf, shim *myshim);
  int (*probe_tags)(uint64_t *buf, shim * myshim);
};


typedef struct {
  //global start
  uint64_t begin[MAX_HW_EVENTS];
  //global end
  uint64_t end[MAX_HW_EVENTS];
  uint64_t nr_samples;
  uint64_t nr_bad_samples;
  uint64_t nr_taken_samples;
  uint64_t nr_interesting_samples;
} shim_stat;

typedef struct {
  int flag;
  int tid;
  int cpu;
  int targetcpu;
  int rate;
  int approach;
  int eventc;
  int buffersize;
  int intelpt;
  char *output_file;
  char **eventv;
  shim_stat avg_stat;
} shim_cmd;


#define debug_print(...) fprintf (stderr, __VA_ARGS__)

static uint64_t __inline__ rdtscp(void)
{
  unsigned int tickl, tickh;
  __asm__ __volatile__("rdtscp":"=a"(tickl),"=d"(tickh)::"%ecx");
  return ((uint64_t)tickh << 32)|tickl;
}

#define PPID_MAP_ELESIZE (64)
static unsigned long __inline__ tid_on_cpu(char *map, int cpu)
{
  return *(unsigned long *)((char *)map + cpu * PPID_MAP_ELESIZE);
}


static void __inline__ bind_processor(int cpu)
{
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu, &cpuset);
  pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

static void __inline__ *get_fsbase()
{
  void *base = NULL;
  __asm__("movq %%fs:0, %0\n\t":"=r"(base):);
  return base;
}
