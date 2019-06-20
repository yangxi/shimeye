#include "shim.h"

hw_event_t *perf_events = NULL;
int default_nr_events = 3;
char *default_counter_name[3] = {"INSTRUCTION_RETIRED:u:t","INSTRUCTION_RETIRED:u","UNHALTED_CORE_CYCLES:u"};
volatile unsigned long *ppid;
volatile int *nr_syscall;

//char *perf_counters[3] = {"L2_RQSTS:REFERENCES:u:t","L2_RQSTS:MISS:u:t","L2_RQSTS:ALL_PF:u:t"};

//probe observes a core with a certain sampling freqnecy, reports performance counter signals,
//PID/TID signals.
//usage: probe coreNumber frequencycounters
//probe recrods signals at the given frequency into a buffer. If the buffer is full, probe
//dump the data to output.
//TODO: feed the data to a ipython drawing script.

unsigned long perf_log[100];


//probe -1 -1 frequency [counters...] monitor all cores from their paired SMT lanes.
//probe -1 [0-NR_CPU) frequency [counters...] monitor all cores from a SMT lanes.
//probe [0-NR_CPU) [0-NR_CPU) frequency [counters...] monitor the target core from the observer thread.


int
main(int argc, char **argv)
{
  if (argc < 4) {
    printf("usage: probe targetcore observeingcore frequency [counters...]\n");
    exit(0);
  }

  int ret = pfm_initialize();
  if (ret != PFM_SUCCESS) {
    err(1,"pfm_initialize() is failed!");
    exit(-1);
  }

  int target_cpu = atoi(argv[1]);
  int running_cpu = atoi(argv[2]);

  int nr_counter = default_nr_events;
  char **counter_names = default_counter_name;
  if (argc > 4) {
    nr_counter = argc - 4;
    counter_names = argv + 4;
  }

  printf("Running on CPU %d, watches target CPU %d.\n", running_cpu, target_cpu);

  bind_processor(running_cpu);
  perf_events = shim_create_hw_events(nr_counter, counter_names);
  //  grab_os_signals(target_cpu, &ppid, &nr_syscall);
  for (int i=0; i<100; i++) {
    int nr_read = read_counters(perf_log, perf_events, nr_counter);
    for (int j=0; j<nr_read; j++){
      printf("%lu,", perf_log[j]);
    }
    printf("\n");
  }
}
