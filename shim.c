#include "shim.h"


//we need os signal to harvest idle cycles

int grab_os_signals(int cpu, unsigned long ** ppid, int ** syscall)
{
  char buf[1024];
  unsigned long signal_phy_addr[32];
  int fd;
  int i;

  if ((fd = open("/sys/module/simple_pt/parameters/shim_signal", O_RDONLY)) < 0){
    fprintf(stderr, "Can't open /sys/module/simple_pt/parameters/shim_signal\n");
    return 1;
  }
  int nr_read = read (fd, buf, 1024);
  debug_print("read %d bytes %s from shim_sginal\n", nr_read, buf);
  signal_phy_addr[0] = atol(buf);
  char *cur = buf;
  for (i=1; i<32; i=i+1){
    while (*(cur++) != ',')
      ;
    signal_phy_addr[i] = atol(cur);
  }
  close(fd);

  unsigned long mmap_offset = signal_phy_addr[cpu * 2] & 0xffffffffffff0000;
  int mmap_size = 0x10000;
  int syscall_offset = signal_phy_addr[cpu * 2] - mmap_offset;
  int task_offset = signal_phy_addr[cpu * 2 + 1] - mmap_offset;
  int mmap_fd;

  if ((mmap_fd = open("/dev/mem", O_RDONLY)) < 0) {
    fprintf(stderr,"Can't open /dev/mem");
    return 1;
  }
  char *mmap_addr = mmap(0, mmap_size, PROT_READ, MAP_SHARED, mmap_fd, mmap_offset);
  if (mmap_addr == MAP_FAILED) {
    fprintf(stderr,"Can't mmap /dev/mem");
    return 1;
  }
  *ppid = (unsigned long *)(mmap_addr + task_offset);
  *syscall = (int *)(mmap_addr + syscall_offset);
  debug_print("mmap /dev/mem on fd:%d, offset 0x%lx, at addr %p, ppid %p, syscall %p\n",
	   mmap_fd, mmap_offset, mmap_addr, *ppid, *syscall);
  return 0;
}

static char *copy_name(char *name)
{
  char *dst = (char *)malloc(strlen(name) + 1);
  strncpy(dst, name, strlen(name) + 1);
  return dst;
}

static void create_hw_event(char *name, hw_event_t *e)
{
  struct perf_event_attr *pe = &(e->perf_attr);
  int ret = pfm_get_perf_event_encoding(name, PFM_PLM3, pe, NULL, NULL);
  if (ret != PFM_SUCCESS) {
    errx(1, "error creating event '%s': %s\n", name, pfm_strerror(ret));
  }
  pe->sample_type = PERF_SAMPLE_READ;
  e->fd = perf_event_open(pe, 0, -1, -1, 0);
  if (e->fd == -1) {
    err(1, "error in perf_event_open for event %s", name);
  }
  //mmap the fd to get the raw index
  e->buf = (struct perf_event_mmap_page *)mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ, MAP_SHARED, e->fd, 0);
  if (e->buf == MAP_FAILED) {
    err(1,"mmap on perf fd %d %s", e->fd, name);
  }

  e->name = copy_name(name);

  e->index = e->buf->index - 1;
  debug_print("Creat hardware event name:%s, fd:%d, index:%x\n",
	      name,
	      e->fd,
	      e->index);
}

hw_event_t * shim_create_hw_events(int nr_hw_events, char **hw_event_names)
{
  //relase old perf events
  int i;
  hw_event_t * hw_events = (hw_event_t *) calloc(nr_hw_events, sizeof(hw_event_t));
  if (hw_events == NULL)
    return NULL;

  for (i=0; i<nr_hw_events; i++){
    create_hw_event(hw_event_names[i], hw_events + i);
  }
  for (i=0;i <nr_hw_events; i++){
    hw_event_t *e = hw_events + i;
    debug_print("updateindex event %s, fd %d, index %x\n", e->name, e->fd, e->buf->index - 1);
    e->index = e->buf->index - 1;
  }
  return hw_events;
}


int read_counters(unsigned long *p, hw_event_t *events, int nr_events)
{
  int ret = 0;

  p[ret++] = rdtsc();
  for (int i=0; i<nr_events; i++){
    rdtsc();
    p[ret++] = __builtin_ia32_rdpmc(events[i].index);
  }
  p[ret++] = rdtsc();
  return ret;
}

int trustable(unsigned long *start, unsigned long *end, int lowpass, int highpass)
{
  int cycle_begin_index = 0;
  int cycle_end_index = 4;
  uint64_t cycle_begin_diff = end[cycle_begin_index] - start[cycle_begin_index];
  uint64_t cycle_end_diff = end[cycle_end_index] - start[cycle_end_index];
  int cpc = (cycle_end_diff * 100 ) / cycle_begin_diff;
  if (cpc < lowpass || cpc > highpass){
    //    debug_print("cpc is %d\n", cpc);
    return 0;
  }
  return 1;
}
