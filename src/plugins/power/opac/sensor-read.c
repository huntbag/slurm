#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <asm/unistd.h>

static int fd = -1;

long perf_event_open( struct perf_event_attr *hw_event, pid_t pid,
                      int cpu, int group_fd, unsigned long flags )
{
    int ret;

    ret = syscall( __NR_perf_event_open, hw_event, pid, cpu,
                   group_fd, flags );
    return ret;
}


unsigned int perf_event_map(char * param){
  unsigned config = 0;
  float factor;
  if(strcmp(param, "current_power")==0){
      factor = 1;
      return 9;
  }
  else if(strcmp(param, "current_pcap")==0){
      factor = 100.0/89.0;
      return 13;
  }
  else if(strcmp(param, "max_pcap")==0){
      factor = 100.0/89.0;
      return 12;
  }
  else if(strcmp(param, "soft_min_pcap")==0){
      factor = 100.0/89.0;
      return 11;
  }
  else if(strcmp(param, "hard_min_pcap")==0){
      factor = 100.0/89.0;
      return 10;
  }

  return config;
}

float perf_event_factor_map(char * param){
  unsigned config = 0;
  float factor;
  if(strcmp(param, "current_power")==0){
      factor = 1;
      return factor;
  }
  else if(strcmp(param, "current_pcap")==0){
      factor = 100.0/89.0;
      return factor;
  }
  else if(strcmp(param, "max_pcap")==0){
      factor = 100.0/89.0;
      return factor;
  }
  else if(strcmp(param, "soft_min_pcap")==0){
      factor = 100.0/89.0;
      return factor;
  }
  else if(strcmp(param, "hard_min_pcap")==0){
      factor = 100.0/89.0;
      return factor;
  }

  return 1;
}


long long perf_event_reader(char * param)
{
  long long ret_count;
  struct perf_event_attr pe;
  long long count;
  float factor = 1;

  memset(&pe, 0, sizeof(struct perf_event_attr));
  printf("value od size:%lu",sizeof(struct perf_event_attr));
  pe.type = 6;
  // pe.size = 48;
  factor = perf_event_factor_map(param);
  pe.config = perf_event_map(param);
  pe.size = sizeof(struct perf_event_attr);
  // pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
  pe.inherit = 1;

  // pe.config = PERF_COUNT_HW_INSTRUCTIONS;
  pe.disabled = 1;
  // pe.exclude_kernel = 1;
  // pe.exclude_hv = 1;

  fd = perf_event_open(&pe, -1/*pid*/, 0/*cpu*/, -1/*group fd*/, 0 /*flags*/);


  if (fd == -1) {
    fprintf(stderr, "Error opening leader %lu\n", pe.config);
    exit(EXIT_FAILURE);
  }

  ioctl(fd, PERF_EVENT_IOC_RESET, 0);
  ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);

  ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
  read(fd, &count, sizeof(long long));

  printf("count %lld value\n", count);
  close(fd);
  ret_count = count*factor;
  return ret_count;
}

void main(){
  long long count;
  count = perf_event_reader("current_pcap");
  printf("ret_count %lld value\n", count);
  count = perf_event_reader("max_pcap");
  printf("ret_max %lld val\n",count);
  return;
}

