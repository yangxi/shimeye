#include "shimpt.h"
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <getopt.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <poll.h>

//shim cmd
//#define DEBUG

static int parse_value(char *cmd_str, char *key, int default_value)
{
  char * val_str = strstr(cmd_str, key);
  if (val_str == NULL)
    return default_value;
  val_str += strlen(key);
  return atoi(val_str);
}

static char * parse_string(char *cmd_str, char *key, char *default_str)
{
  char * val_str = strstr(cmd_str, key);
  if (val_str == NULL)
    return default_str;
  val_str += strlen(key);
  int size = 0;
  while(val_str[size] != ';' && val_str[size] != '\0')
    size++;
  char *ret = (char *)calloc(size, 1);
  if (ret == NULL)
    return ret;
  memcpy(ret, val_str, size);
  return ret;
}



//return number of event strings,
//strings are copied in eventv
static void parse_hw_event(char *cmd_str, shim_cmd * cmd)
{

  char *val_str = strstr(cmd_str, "hwevent:");
  if (val_str == NULL)
    return;
  val_str += sizeof("hwevent:") - 1;
  if (val_str[0] == '\0' || val_str[0] == ';')
    return;
  int event_str_size = 0;
  int eventc = 0;
  while (val_str[event_str_size] != '\0' && val_str[event_str_size] != ';'){
    if (val_str[event_str_size] == ',')
      eventc += 1;
    event_str_size += 1;
  }
  eventc += 1;
  char *event_str = (char *)calloc(1, event_str_size + 1);
  char **eventv = (char **)calloc(eventc, sizeof(char *));
  if (event_str == NULL || eventv == NULL){
    if (event_str != NULL)
      free(event_str);
    if (eventv != NULL)
      free(eventv);
    return;
  }
  strncpy(event_str, val_str, event_str_size);

  eventv[0] = event_str;
  int i = 0;
  int event_index = 0;
  for (i=0, event_index = 1; i<event_str_size; i++){
    if (event_str[i] == ',') {
      event_str[i] = '\0';
      eventv[event_index++] = &event_str[i+1];
    }
  }

  //init cmd
  cmd->eventc = eventc;
  cmd->eventv = eventv;
}

//tid:1234;rate:100;cpu:3;targetcpu:4;how:0;hwevent:EVENT0,EVENT1,...;
static shim_cmd * parse_shim_cmd(char *cmd_str)
{
  int i;
  shim_cmd *cmd = (shim_cmd *)calloc(1, sizeof(shim_cmd));
  if (cmd == NULL)
    return NULL;
  cmd->flag = parse_value(cmd_str, "flag:", 0);
  cmd->tid = parse_value(cmd_str, "tid:", -1);
  cmd->intelpt = parse_value(cmd_str, "intelpt:", 0);
  cmd->cpu = parse_value(cmd_str, "cpu:", -1);
  cmd->targetcpu = parse_value(cmd_str, "targetcpu:", -1);
  cmd->rate = parse_value(cmd_str, "rate:", 1);
  cmd->approach = parse_value(cmd_str, "how:", 0);
  cmd->buffersize = parse_value(cmd_str, "bufsize:", SHIM_BUFFERSIZE);
  cmd->output_file = parse_string(cmd_str, "output:", "/tmp/shimpt.out");
  parse_hw_event(cmd_str, cmd);
  debug_print("flag:%d, tid:%d, intelpt:%d, cpu:%d, targetcpu:%d, rate:%d, how:%d, bufsize:%d, output:%s\n",
	      cmd->flag, cmd->tid, cmd->intelpt, cmd->cpu, cmd->targetcpu, cmd->rate, cmd->approach, cmd->buffersize, cmd->output_file);

  for (i=0; i<cmd->eventc; i++){
    debug_print("cmd: event%d:%s\n", i, cmd->eventv[i]);
  }
  return cmd;
}

static void free_shim_cmd(shim_cmd *cmd)
{
  if (cmd){
    if (cmd->eventv){
      if (cmd->eventv[0])
	free(cmd->eventv[0]);
      free(cmd->eventv);
    }
    free(cmd);
  }
}

/* static void write_pt_flag(char *cmd, int size) */
/* { */
/*   int fd = open("/sys/module/simple_pt/parameters/start", O_WRONLY); */
/*   if (fd == -1) */
/*     err(1, "can't open the file\n"); */
/*   int n = write(fd, cmd, size); */
/*   close(fd); */
/*   debug_print("write %d bytes, %s\n", n, cmd); */
/* } */

/* //static void turn_on_pt(void) */
/* //{ */
/* //  write_pt_flag("1",2); */
/* //} */

/* static void turn_off_pt(void) */
/* { */
/*   write_pt_flag("0",2); */
/* } */

//entry of shim profiler

struct option opts[] = {
  	{ "intelpt", no_argument, NULL, 'i' },
	{ "output", required_argument, NULL, 'o' },
	{ }
};




//help functions
static char *copy_name(char *name)
{
  char *dst = (char *)malloc(strlen(name) + 1);
  strncpy(dst, name, strlen(name) + 1);
  return dst;
}

int map_signal_phy_address(int cpuid, unsigned long *signal_addr, unsigned long **signal)
{

  unsigned long phy_addr = signal_addr[cpuid];
  unsigned long mmap_offset = phy_addr & ~(0x1000 - 1);
  int signal_offset = phy_addr & (0x1000 - 1);
  int mmap_size = 0x1000;
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
  *signal = (unsigned long *)(mmap_addr + signal_offset);
  debug_print("map cpu%d signal_addr:0x%lx to virtual_addr:%p\n", cpuid, phy_addr, *signal);
  return 0;
}

int fetch_signal_phy_address(char *path, int nr_cpu, unsigned long * signal_addr)
{
  int i;
  int fd;
  char buf[1024];
  if ((fd = open(path, O_RDONLY)) < 0){
    fprintf(stderr, "Can't open signal address file %s\n", path);
    return 1;
  }
  int nr_read = read (fd, buf, 1024);
  debug_print("read %d bytes %s from shim_sginal\n", nr_read, buf);
  signal_addr[0] = atol(buf);
  char *cur = buf;
  for (i=1; i<nr_cpu; i=i+1){
    while (*(cur++) != ',')
      ;
    unsigned long phyaddr = atol(cur);
    signal_addr[i] = phyaddr;
  }
  close(fd);
  return 0;
}


int grab_signals(int cpu, unsigned long ** ppid, int ** syscall)
{
  char buf[1024];
  unsigned long signal_phy_addr[32];
  int fd;
  int i;

  if ((fd = open("/sys/module/ksignal/parameters/task_signal", O_RDONLY)) < 0){
    fprintf(stderr, "Can't open /sys/module/ksignal/parameters/task_signal\n");
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


static void shim_create_hw_event(char *name, int id, shim *myshim)
{
  struct shim_hardware_event * event = myshim->hw_events + id;
  struct perf_event_attr *pe = &(event->perf_attr);
  int ret = pfm_get_perf_event_encoding(name, PFM_PLM3, pe, NULL, NULL);
  if (ret != PFM_SUCCESS) {
    errx(1, "error creating event %d '%s': %s\n", id, name, pfm_strerror(ret));
  }
  pe->sample_type = PERF_SAMPLE_READ;
  event->fd = perf_event_open(pe, 0, -1, -1, 0);
  if (event->fd == -1) {
    err(1, "error in perf_event_open for event %d '%s'", id, name);
  }
  //mmap the fd to get the raw index
  event->buf = (struct perf_event_mmap_page *)mmap(NULL, sysconf(_SC_PAGESIZE), PROT_READ, MAP_SHARED, event->fd, 0);
  if (event->buf == MAP_FAILED) {
    err(1,"mmap on perf fd");
  }

  event->name = copy_name(name);

  event->index = event->buf->index - 1;
  debug_print("SHIM %d:creat %d hardware event name:%s, fd:%d, index:%x\n",
	      myshim->cpuid,
	      id,
	      name,
	      event->fd,
	      event->index);
}

static void shim_create_hwsignals(shim *my, int nr_hw_events, char **hw_event_names)
{
  //relase old perf events
  int i;

  memset(my->hw_events, 0, sizeof(my->hw_events));
  my->nr_hw_events = nr_hw_events;
  //  assert(my->hw_events != NULL);
  for (i=0; i<nr_hw_events; i++){
    shim_create_hw_event(hw_event_names[i], i, my);
  }
  for (i=0;i <nr_hw_events; i++){
    struct shim_hardware_event *e = my->hw_events + i;
    debug_print("updateindex event %s, fd %d, index %x\n", e->name, e->fd, e->buf->index - 1);
    e->index = e->buf->index - 1;
    my->pmc_index[i] = e->index;
  }
  my->pmc_index[nr_hw_events] = -1;
}

static char dump_str[1024];

shim * profiler;
int outfd = -1;

//ts(long),event0(int)...eventN(int),syscall(int),pid(long),ts(long)
static void shimpt_read_counter(unsigned int *buf)
{
  int a,b;
  __asm__ __volatile__("rdtscp\n\t"
  		       "movnti %%eax, (%%rsi)\n\t"
  		       "movnti %%edx, 4(%%rsi)\n\t"
  		       "addq $8, %%rsi\n\t"
  		       "0:\n\t"
  		       "movl (%%rdi), %%ecx\n\t"
  		       "cmpl $-1, %%ecx\n\t"
  		       "je 1f\n\t"
  		       "rdpmc\n\t"
  		       "movl %%eax, (%%rsi)\n\t"
  		       "addq $4, %%rsi\n\t"
  		       "addq $4, %%rdi\n\t"
  		       "jmp 0b\n\t"
  		       "1:\n\t"
		       "movl (%5), %%ecx\n\t"
		       "movntil %%ecx, (%%rsi)\n\t"
  		       "movq (%4), %%rcx\n\t"
  		       "movntiq %%rcx, 4(%%rsi)\n\t"
  		       "rdtscp\n\t"
  		       "movntil %%eax, 12(%%rsi)\n\t"
  		       "movntil %%edx, 16(%%rsi)\n\t"
  		       :"+a"(a),"+d"(b):"S"(buf),"D"(profiler->pmc_index),"r"(profiler->ppid_source),"r"(profiler->syscall_source):"%ecx","memory");
}

//ts(long),event0(int)...eventN(int),syscall(int),pid(long),ts(long)
void debug_dump_log(char *buf)
{
  int i;
  fprintf(stderr,"[%lx",*((unsigned long*)buf));
  buf += sizeof(unsigned long);
  for (i=0;i<profiler->nr_hw_events; i++){
    fprintf(stderr,",%u", *((unsigned int *)(buf + i * sizeof(unsigned int))));
  }
  buf += profiler->nr_hw_events * sizeof(unsigned int);
  fprintf(stderr,"%d,,%u,%u,%lx]\n",*(int *)buf, *(unsigned int*)(buf + sizeof(int)), *(unsigned int*)(buf + 2*sizeof(unsigned int)), *(unsigned long*)(buf + 3*sizeof(unsigned int)));
}

int main(int argc, char **argv)
{

  if (argc != 2){
    fprintf(stderr, "Wrong parameters\n");
    exit(1);
  }
  int ret = pfm_initialize();
  if (ret != PFM_SUCCESS) {
    err(1,"pfm_initialize() is failed!");
    exit(-1);
  }

  profiler = (shim *)calloc(1, sizeof(shim));
  if (profiler == NULL){
    perror("Can't alloc shim\n");
    exit(1);
  }
  shim_cmd *cmd = parse_shim_cmd(argv[1]);
  outfd = creat(cmd->output_file, 0666);
  if (outfd == -1){
    fprintf(stderr, "Can't open file %s\n", cmd->output_file);
    exit(1);
  }
  shim_create_hwsignals(profiler, cmd->eventc, cmd->eventv);
  char *buf = (char *)malloc(cmd->buffersize);
  if (buf == NULL){
    fprintf(stderr, "Can't alloc %d for buf\n", cmd->buffersize);
    exit(1);
  }

  debug_print("Bind to cpu %d\n", cmd->cpu);
  bind_processor(cmd->cpu);

  int i;
  //  for (i=0;i<MAX_HW_EVENTS;i++)
  //    debug_print("PMC index%d:%x\n", i, profiler->pmc_index[i]);
  if (grab_signals(cmd->targetcpu, &(profiler->ppid_source), &(profiler->syscall_source))){
    fprintf(stderr,"Can't find software signals\n");
    exit(1);
  }

  memset(buf, 0,cmd->buffersize);

  char *cur = buf;
  char *end = buf + cmd->buffersize;
  //ts(long),event0(int)...eventN(int),pid(long),ts(long)
  int log_size = 3 * sizeof(long) + (profiler->nr_hw_events + 1)* sizeof(int);
  debug_print("Log size is %d, start to profile\n", log_size);
  shimpt_read_counter((unsigned int *)(cmd->avg_stat.begin));
  debug_dump_log(cmd->avg_stat.begin);
//  if (cmd->intelpt)
//    turn_on_pt();
  while (cur + log_size < end){
    shimpt_read_counter((unsigned int *)cur);
#ifdef SHIM_DEBUG
    debug_dump_log(cur);
#endif
    cur += log_size;
  }
  /* if (cmd->intelpt) */
  /*   turn_off_pt(); */
  shimpt_read_counter((unsigned int*)(cmd->avg_stat.end));
  //output
  //metadata first

  char * dump_str_cur = dump_str;
  dump_str_cur += sprintf(dump_str_cur,"#tsb->long");

  for (i=0 ;i<profiler->nr_hw_events; i++){
    dump_str_cur  += sprintf(dump_str_cur,",%s->int",profiler->hw_events[i].name);
  }
  dump_str_cur += sprintf(dump_str_cur,",syscall->int,tid->int,pid->int,tse->long\n");
  write(outfd, dump_str, dump_str_cur - dump_str);
  debug_print("Dump %ld bytes\n", write(outfd, buf, cur-buf));
  close(outfd);
  return 0;
}
