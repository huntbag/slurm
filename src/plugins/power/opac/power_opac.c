
/*****************************************************************************\
 *  power_opac.c - Plugin for "IBM-power systems" power management. Open Power Aware Cluster (OPAC)

    This code uses similar logic as implemented in cray plugin in the file power_cray.c for power management. We are using impitool to set the power limit on each node. We are extracting the values of the parameter required by using perf events.

*****************************************************************************
 *  Copyright (C) 2014-2015 SchedMD LLC.
 *  Copyright (C) 2016 IBM Corporation
 *  Written by Morris Jette <jette@schedmd.com>
 *
 *  This file is part of SLURM, a resource management program.
 *  For details, see <http://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  SLURM is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  SLURM is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with SLURM; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/
#if HAVE_CONFIG_H
#include "config.h"
#endif

#define _GNU_SOURCE	/* For POLLRDHUP */
#include <ctype.h>
#include <poll.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h> 

#include <inttypes.h>

#include "slurm/slurm.h"

#include "src/plugins/power/common/power_common.h"
#include "src/plugins/power/opac/sensor-read.c"


#define DEFAULT_BALANCE_INTERVAL  30
#define DEFAULT_CAP_WATTS         3000
#define DEFAULT_DECREASE_RATE     50
#define DEFAULT_GET_TIMEOUT       5000
#define DEFAULT_INCREASE_RATE     20
#define DEFAULT_LOWER_THRESHOLD   90
#define DEFAULT_SET_TIMEOUT       30000
#define DEFAULT_UPPER_THRESHOLD   95
#define DEFAULT_RECENT_JOB        300
#define DEFAULT_GET_TIMEOUT       5000

#define SOCKET_COMM_PORT 	  5000

/*
 * These variables are required by the generic plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  SLURM uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "burst_buffer" for SLURM burst_buffer) and <method> is a
 * description of how this plugin satisfies that application.  SLURM will only
 * load a burst_buffer plugin if the plugin_type string has a prefix of
 * "burst_buffer/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */

const char plugin_name[]        = "power opac plugin";
const char plugin_type[]        = "power/opac";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;


static pthread_t power_thread = 0;
static pthread_mutex_t thread_flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t opac_power_server;


static void _load_config(void);
//static void _log_node_power(void);
static void populate_power_data(void);
extern void *_power_agent(void *args);
static void *_power_gathering_engine(void *arg);
static void _spawn_power_gathering_engine(void);
static void stop_power_agent(void);

/* config params */
static int balance_interval = DEFAULT_BALANCE_INTERVAL;
static uint32_t cap_watts = DEFAULT_CAP_WATTS;
static uint32_t set_watts = 0;
static uint64_t debug_flag = 0;
static uint32_t decrease_rate = DEFAULT_DECREASE_RATE;
static uint32_t increase_rate = DEFAULT_INCREASE_RATE;
static uint32_t job_level = NO_VAL;
//static time_t last_cap_read = 0;
static time_t last_limits_read = 0;
static uint32_t lower_threshold = DEFAULT_LOWER_THRESHOLD;
static uint32_t recent_job = DEFAULT_RECENT_JOB;
static uint32_t upper_threshold = DEFAULT_UPPER_THRESHOLD;
static bool stop_power = false;
static int get_timeout = DEFAULT_GET_TIMEOUT;
static int set_timeout = DEFAULT_SET_TIMEOUT;

enum power_parameters { get_caps, set_caps, get_current_usage, get_max_watts, get_min_watts, check_node_status, get_phase, all };

int socket_fd;
extern struct node_record *node_record_table_ptr;

/* _load_config function initializes the power parameters by loading it from the slurm.conf */
static void _load_config(void)
{
	char *end_ptr = NULL, *sched_params, *tmp_ptr;

	debug_flag = slurm_get_debug_flags();
	sched_params = slurm_get_power_parameters();
	if (!sched_params)
		sched_params = xmalloc(1);	/* Set defaults below */

	/*                                   12345678901234567890 */
	if ((tmp_ptr = strstr(sched_params, "balance_interval="))) {
		balance_interval = atoi(tmp_ptr + 17);
		if (balance_interval < 1) {
			error("PowerParameters: balance_interval=%d invalid",
			      balance_interval);
			balance_interval = DEFAULT_BALANCE_INTERVAL;
		}
	} else {
		balance_interval = DEFAULT_BALANCE_INTERVAL;
	}

	/*                                   12345678901234567890 */
	if ((tmp_ptr = strstr(sched_params, "cap_watts="))) {
		cap_watts = strtol(tmp_ptr + 10, &end_ptr, 10);
		if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
			cap_watts *= 1000;
		} else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			cap_watts *= 1000000;
		}
	} else {
		cap_watts = DEFAULT_CAP_WATTS;
	}

	if ((tmp_ptr = strstr(sched_params, "decrease_rate="))) {
		decrease_rate = atoi(tmp_ptr + 14);
		if (decrease_rate < 1) {
			error("PowerParameters: decrease_rate=%u invalid",
			      balance_interval);
			lower_threshold = DEFAULT_DECREASE_RATE;
		}
	} else {
		decrease_rate = DEFAULT_DECREASE_RATE;
	}

	if ((tmp_ptr = strstr(sched_params, "increase_rate="))) {
		increase_rate = atoi(tmp_ptr + 14);
		if (increase_rate < 1) {
			error("PowerParameters: increase_rate=%u invalid",
			      balance_interval);
			lower_threshold = DEFAULT_INCREASE_RATE;
		}
	} else {
		increase_rate = DEFAULT_INCREASE_RATE;
	}

	if (strstr(sched_params, "job_level"))
		job_level = 1;
	else if (strstr(sched_params, "job_no_level"))
		job_level = 0;
	else
		job_level = NO_VAL;

	if ((tmp_ptr = strstr(sched_params, "get_timeout="))) {
		get_timeout = atoi(tmp_ptr + 12);
		if (get_timeout < 1) {
			error("PowerParameters: get_timeout=%d invalid",
			      get_timeout);
			get_timeout = DEFAULT_GET_TIMEOUT;
		}
	} else {
		get_timeout = DEFAULT_GET_TIMEOUT;
	}

	if ((tmp_ptr = strstr(sched_params, "lower_threshold="))) {
		lower_threshold = atoi(tmp_ptr + 16);
		if (lower_threshold < 1) {
			error("PowerParameters: lower_threshold=%u invalid",
			      lower_threshold);
			lower_threshold = DEFAULT_LOWER_THRESHOLD;
		}
	} else {
		lower_threshold = DEFAULT_LOWER_THRESHOLD;
	}

	if ((tmp_ptr = strstr(sched_params, "recent_job="))) {
		recent_job = atoi(tmp_ptr + 11);
		if (recent_job < 1) {
			error("PowerParameters: recent_job=%u invalid",
			      recent_job);
			recent_job = DEFAULT_RECENT_JOB;
		}
	} else {
		recent_job = DEFAULT_RECENT_JOB;
	}

	if ((tmp_ptr = strstr(sched_params, "set_timeout="))) {
		set_timeout = atoi(tmp_ptr + 12);
		if (set_timeout < 1) {
			error("PowerParameters: set_timeout=%d invalid",
			      set_timeout);
			set_timeout = DEFAULT_SET_TIMEOUT;
		}
	} else {
		set_timeout = DEFAULT_SET_TIMEOUT;
	}

	if ((tmp_ptr = strstr(sched_params, "set_watts="))) {
		set_watts = strtol(tmp_ptr + 10, &end_ptr, 10);
		if ((end_ptr[0] == 'k') || (end_ptr[0] == 'K')) {
			set_watts *= 1000;
		} else if ((end_ptr[0] == 'm') || (end_ptr[0] == 'M')) {
			set_watts *= 1000000;
		}
	} else {
		set_watts = 0;
	}

	if ((tmp_ptr = strstr(sched_params, "upper_threshold="))) {
		upper_threshold = atoi(tmp_ptr + 16);
		if (upper_threshold < 1) {
			error("PowerParameters: upper_threshold=%u invalid",
			      upper_threshold);
			upper_threshold = DEFAULT_UPPER_THRESHOLD;
		}
	} else {
		upper_threshold = DEFAULT_UPPER_THRESHOLD;
	}

	xfree(sched_params);

	if (debug_flag & DEBUG_FLAG_POWER) {
		char *level_str = "";
		if (job_level == 0)
			level_str = "job_no_level,";
		else if (job_level == 1)
			level_str = "job_level,";
		debug("PowerParameters=balance_interval=%d,"
		     "cap_watts=%u,decrease_rate=%u,get_timeout=%d,"
		     "increase_rate=%u,%slower_threshold=%u,recent_job=%u,"
		     "set_timeout=%d,set_watts=%u,upper_threshold=%u",
		     balance_interval, cap_watts, decrease_rate,
		     get_timeout, increase_rate, level_str, lower_threshold,
		     recent_job, set_timeout, set_watts, upper_threshold);
	}

	last_limits_read = 0;	/* Read node power limits again */
}

/* function to log the power data of the nodes present in the cluster

static void _log_node_power(void)
{
	struct node_record *node_ptr;
	uint32_t total_current_watts = 0, total_min_watts = 0;
	uint32_t total_max_watts = 0, total_cap_watts = 0;
	uint32_t total_new_cap_watts = 0, total_ready_cnt = 0;
	int i;

	// Build and log summary table of required updates to power caps 
	for (i = 0, node_ptr = node_record_table_ptr; i < node_record_count;
	     i++, node_ptr++) {
		char *ready_str;
		if (!node_ptr->power)
			continue;
		if (node_ptr->power->state == 1) {
			ready_str = "YES";
			total_ready_cnt++;
		} else
			ready_str = "NO";
		info("Node:%s CurWatts:%3u MinWatts:%3u "
		     "MaxWatts:%3u OldCap:%3u NewCap:%3u Ready:%s",
		     node_ptr->name, node_ptr->power->current_watts,
		     node_ptr->power->min_watts,
		     node_ptr->power->max_watts,
		     node_ptr->power->cap_watts,
		     node_ptr->power->new_cap_watts, ready_str);
		total_current_watts += node_ptr->power->current_watts;
		total_min_watts     += node_ptr->power->min_watts;
		total_max_watts     += node_ptr->power->max_watts;
		if (node_ptr->power->cap_watts)
			total_cap_watts     += node_ptr->power->cap_watts;
		else
			total_cap_watts     += node_ptr->power->max_watts;
		if (node_ptr->power->new_cap_watts)
			total_new_cap_watts += node_ptr->power->new_cap_watts;
		else if (node_ptr->power->cap_watts)
			total_new_cap_watts += node_ptr->power->cap_watts;
		else
			total_new_cap_watts += node_ptr->power->max_watts;
	}
	info("TOTALS CurWatts:%u MinWatts:%u MaxWatts:%u OldCap:%u "
	     "NewCap:%u ReadyCnt:%u",
	     total_current_watts, total_min_watts, total_max_watts,
	     total_cap_watts, total_new_cap_watts, total_ready_cnt);
}

*/

/* server socket code - that takes a power parameter and returns the respective power details */
static void
_spawn_power_gathering_engine(void)
{
        debug3("slurmd spawing the slurmd power accumulation engine");
        int rc;
        pthread_attr_t attr;
        int retries = 0;

        slurm_attr_init(&attr);
        rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        if (rc != 0) {
                errno = rc;
                fatal("Unable to set detachstate on attr: %m");
                slurm_attr_destroy(&attr);
                return;
        }

        while (pthread_create(&opac_power_server, &attr, &_power_gathering_engine, NULL)) {
                error("power_gathering_engine: pthread_create: %m");
                if (++retries > 3)
                        fatal("msg_engine: pthread_create: %m");
                usleep(10);     /* sleep and again */
        }
        debug3("slurmd created pthread - power accumulation / gathering ");

        return;
}
//int master = 0;
static void *
_power_gathering_engine(void *arg)
{


        debug3("started the power accumulation pthread server - to capture the power data");

        debug3("here run's the slurm compute daemon server for collecting the power information");

        int listenfd = 0, connfd = 0, num;
        struct sockaddr_in serv_addr;

        char buffer[1024];
        char sendBuff[1024];

        listenfd = socket(AF_INET, SOCK_STREAM, 0);
        memset(&serv_addr, '0', sizeof(serv_addr));
        memset(sendBuff, '0', sizeof(sendBuff));

        serv_addr.sin_family = AF_INET;
        serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        serv_addr.sin_port = htons(SOCKET_COMM_PORT);

        bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

        listen(listenfd, 10);

        debug("server listening to the port for client requests");

        while(1) {
		
                connfd = accept(listenfd, (struct sockaddr*)NULL, NULL);
                if ((num = recv(connfd, buffer, 1024,0))== -1) {
	                debug("error while recv power parameter from client");
                }
                else if (num == 0) {
                        debug("connection closed");
                }
                buffer[num] = '\0';
                debug("server: msg received %s", buffer);
	
		/* depending upon the request received send the information */
		
		/*TODO :  here goes the server logic  push the value in send buffer and send it to the client */
		
		switch(buffer[0]) {

			case '0': {
                                uint64_t power = (uint64_t)perf_event_reader("current_pcap");
 //                              	uint64_t power = 1300;
				debug("info :  current node power caps : %"PRIu64, power);   

                                snprintf(sendBuff, sizeof(sendBuff)," %"PRIu64"\r\n", power);
                                write(connfd, sendBuff, strlen(sendBuff));
                                debug("server: Msg being sent: %s Number of bytes sent: %"PRIu64,sendBuff, strlen(sendBuff));
                                break;
                        }


			case '1': {

				char *cmd_resp, *script_argv[6];
				static char *ipmi_path = "/usr/bin/ipmitool";
				script_argv[0] = ipmi_path;
				script_argv[1] = "dcmi";
				script_argv[2] = "power";
				script_argv[3] = "set_limit";
				script_argv[4] = "limit";
				//debug("info :copy the value of buffer into scrpt argv %s\n", buffer+1);
				script_argv[5] = (char*)malloc(strlen(buffer+1)+2);
				strcpy(script_argv[5],buffer+1);
				//debug("info : copied string successfully");
//				script_argv[5] = "1300";
				script_argv[6] = NULL;

				int status = 0;
				static int get_timeout = DEFAULT_GET_TIMEOUT;

				cmd_resp = power_run_script("ipmitool", ipmi_path, script_argv,
				    get_timeout, NULL, &status);
			
				debug("POWER: INFO : ipmitool command response: %s , %d",cmd_resp,status);			
				
				script_argv[3] = "activate";
				script_argv[4] = NULL;
			
				cmd_resp = power_run_script("ipmitool", ipmi_path, script_argv,
                                    get_timeout, NULL, &status);

                                debug("POWER: INFO : ipmitool activate command response: %s , %d",cmd_resp,status);

				break;			

			}

			case '2': {
				uint64_t power = (uint64_t)perf_event_reader("current_power");
//				uint64_t power = 1400;
				debug("info :  current node power consumption : %"PRIu64, power); 	
				snprintf(sendBuff, sizeof(sendBuff)," %"PRIu64"\r\n", power);
	                	write(connfd, sendBuff, strlen(sendBuff));
        		        debug("server: Msg being sent: %s Number of bytes sent: %"PRIu64,sendBuff, strlen(sendBuff));
				break;
			}

			case '3': {
                                uint64_t power = (uint64_t)perf_event_reader("max_pcap");
                	//	uint64_t power = 1800;
  
		                debug("info :  current node max watts : %"PRIu64, power);
   
                                snprintf(sendBuff, sizeof(sendBuff)," %"PRIu64"\r\n", power);
                                write(connfd, sendBuff, strlen(sendBuff));
                                debug("server: Msg being sent: %s Number of bytes sent: %"PRIu64,sendBuff, strlen(sendBuff));
                                break;
                        }

			case '4': {
                                uint64_t power = (uint64_t)perf_event_reader("soft_min_pcap");
                                                           //uint64_t power = 1100;
					
				debug("info :  current node min watts : %"PRIu64, power);   

                                snprintf(sendBuff, sizeof(sendBuff)," %"PRIu64"\r\n", power);
                                write(connfd, sendBuff, strlen(sendBuff));
                                debug("server: Msg being sent: %s Number of bytes sent: %"PRIu64,sendBuff, strlen(sendBuff));
                                break;
                        }


			case '5': {
                                uint64_t node_state = (uint64_t)1;
                                debug("info :  current node staus : %"PRIu64, node_state);   
                                snprintf(sendBuff, sizeof(sendBuff)," %"PRIu64"\r\n", node_state);
                                write(connfd, sendBuff, strlen(sendBuff));
                                debug("server: Msg being sent: %s Number of bytes sent: %"PRIu64,sendBuff, strlen(sendBuff));
                                break;
                        }

		}

        	close(connfd);
	}

        return NULL;
}
/* end of server power function */

/*opac - naive hack to get the power data */
int hostname_to_ip(char * hostname , char* ip)
{
        struct hostent *he;
        struct in_addr **addr_list;
        int i;

        if ( (he = gethostbyname( hostname ) ) == NULL)
        {
                // get the host info
                debug("get host by name");
                return 1;
        }

        addr_list = (struct in_addr **) he->h_addr_list;

        for(i = 0; addr_list[i] != NULL; i++)
        {
                //Return the first one;
                strcpy(ip , inet_ntoa(*addr_list[i]));
                return 0;
        }

        size_t length = sizeof(ip)/sizeof(*ip);
        ip[length] = '\0';

        return 1;
}

uint64_t get_host_power_info(char *host_name, int power_param) {


	struct sockaddr_in serv_addr;
	struct hostent *he;	
	
	//char buffer[1024];
	char recvBuff[1024];

	int num;

	if((he = gethostbyname(host_name)) == NULL) { 
		debug("Cannot get the host ip address by hostname ");
		exit(1);
	}

	if((socket_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		debug("Socket failure! ");
		exit(1);
	}
	
	memset(&serv_addr, 0 , sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(5000);
	serv_addr.sin_addr = *((struct in_addr *) he->h_addr);

	if( connect(socket_fd, (struct sockaddr *)&serv_addr, sizeof(struct sockaddr)) < 0){
                debug("Error : Connect Failed :%s",__func__);
		exit(1);
        }

	char power_param_buf[5];

//        power_param_buf[1] = '\0';
	sprintf(power_param_buf, "%d", power_param);

	if ((send(socket_fd,power_param_buf, strlen(power_param_buf),0))== -1) {
                fprintf(stderr, "Failure Sending Message\n");
                close(socket_fd);
                exit(1);
        } else {
		while ( (num = read(socket_fd, recvBuff, sizeof(recvBuff)-1)) > 0){
                        recvBuff[num] = 0;
                        if(fputs(recvBuff, stdout) == EOF)
                        {
                                debug("Error : Fputs error: %s",__func__);
                        }
                }
                if(num < 0){
                        debug("Read error : %s",__func__);

                }
	}	

	uint64_t power_data =  (uint64_t) strtoll(recvBuff, NULL, 10);
        debug("info: power_data value %" PRIu64, power_data);
  	
	close(socket_fd);
	return power_data;
}

static void my_sleep(int add_secs)
{
        struct timespec ts = {0, 0};
        struct timeval  tv = {0, 0};

        if (gettimeofday(&tv, NULL)) {          /* Some error */
                sleep(1);
                return;
        }

        ts.tv_sec  = tv.tv_sec + add_secs;
        ts.tv_nsec = tv.tv_usec * 1000;
}


/*populate the power data in the node_record datastructure */
static void populate_power_data(void) {

	//time_t now;
	//double wait_time;
	//static time_t last_balance_time = 0;
	struct node_record *node_ptr;
	int i;
	//last_balance_time = time(NULL);
	while (!stop_power) {
		my_sleep(1);	
		if (stop_power)
			break;
		
	//	now = time(NULL);
	//      wait_time = difftime(now, last_balance_time);
	//	if (wait_time < balance_interval)
	//		continue;
	//	if (wait_time > 300){
			
			for(i=0, node_ptr = node_record_table_ptr; i < node_record_count; i++, node_ptr++) {
				debug("-----INFO : POWER: NODE NAME : %s-----" , node_ptr->name);

			}
			
			
			for(i=0, node_ptr = node_record_table_ptr; i < node_record_count; i++, node_ptr++){
				debug("------INFO : POWER :node name (getting data from the node): %s ------" , node_ptr->name);		
				if(!node_ptr->power)
	                      		node_ptr->power = xmalloc(sizeof(power_mgmt_data_t));

				uint64_t current_power = get_host_power_info(node_ptr->name , get_current_usage);

				node_ptr->power->current_watts = current_power;
			
				debug("INFO: POWER: setting the current power usage of %s to %" PRIu64 " in the node_record data-structure", node_ptr->name , current_power);


				uint64_t current_caps = get_host_power_info(node_ptr->name , get_caps);
				node_ptr->power->cap_watts = current_caps;
	
				debug("INFO: POWER: setting the current power caps of %s to %" PRIu64 " in the node_record data-structure", node_ptr->name , current_caps);


				uint64_t current_max_watts = get_host_power_info(node_ptr->name , get_max_watts);
                		node_ptr->power->max_watts = current_max_watts;

	                	debug("INFO: POWER: setting the current max watts of %s to %" PRIu64 " in the node_record data-structure", node_ptr->name , current_max_watts);

				uint64_t current_min_watts = get_host_power_info(node_ptr->name , get_min_watts);
        	        	node_ptr->power->min_watts = current_min_watts;

	        	        debug("INFO: POWER: setting the current power min watts of %s to %" PRIu64 " in the node_record data-structure", node_ptr->name , current_min_watts);

				uint64_t node_state = get_host_power_info(node_ptr->name , check_node_status);
	                	node_ptr->power->state = node_state;

		                debug("INFO: POWER: setting the current node state of %s to %" PRIu64 " in the node_record data-structure", node_ptr->name , node_state);

			}
	//	}
	
		uint32_t alloc_power = 0, avail_power = 0, ave_power, new_cap, tmp_u32;
        	uint32_t node_power_raise_cnt = 0, node_power_needed = 0;
       		uint32_t node_power_same_cnt = 0, node_power_lower_cnt = 0;
	        time_t recent = time(NULL) - recent_job;

		for(i=0, node_ptr = node_record_table_ptr; i < node_record_count; i++, node_ptr++) {

			if (!node_ptr->power)
                        	continue;
	                if (node_ptr->power->state != 1) {  // Not ready -> no change 
          	              if (node_ptr->power->cap_watts == 0) {
           	                     node_ptr->power->new_cap_watts =
                                        node_ptr->power->max_watts;
                	    } else {
                              	node_ptr->power->new_cap_watts =
                                	node_ptr->power->cap_watts;
                               }
	                       alloc_power += node_ptr->power->new_cap_watts;
        	                continue;
	                }
        	        node_ptr->power->new_cap_watts = 0;
                	if (node_ptr->power->new_job_time >= recent) {
                        	node_power_raise_cnt++; // Reset for new job below 
	                        continue;
        	        }
                	if ((node_ptr->power->cap_watts == 0) ||   // Not initialized 
                     	(node_ptr->power->current_watts == 0)) {
                        	node_power_raise_cnt++; // Reset below 
	                        continue;
        	        }
               		if (node_ptr->power->current_watts <
                    	(node_ptr->power->cap_watts * lower_threshold/100)) {
                        // Lower cap by lower of
                         // 1) decrease_rate OR
                         // 2) half the excess power in the cap 
                        	ave_power = (node_ptr->power->cap_watts -
                                     node_ptr->power->current_watts) / 2;
	                        tmp_u32 = node_ptr->power->max_watts -
        	                          node_ptr->power->min_watts;
                	        tmp_u32 = (tmp_u32 * decrease_rate) / 100;
	                        new_cap = node_ptr->power->cap_watts -
        	                          MIN(tmp_u32, ave_power);
                	        node_ptr->power->new_cap_watts =
                        	        MAX(new_cap, node_ptr->power->min_watts);
	                        alloc_power += node_ptr->power->new_cap_watts;
        	                node_power_lower_cnt++;
                	} else if (node_ptr->power->current_watts <=
                         	  (node_ptr->power->cap_watts * upper_threshold/100)) {
	                        // In desired range. Retain previous cap 
        	                node_ptr->power->new_cap_watts =
                	                MAX(node_ptr->power->cap_watts,
                        	            node_ptr->power->min_watts);
	                        alloc_power += node_ptr->power->new_cap_watts;
        	                node_power_same_cnt++;
               		} else {
                        	// Node should get more power 
	                        node_power_raise_cnt++;
        	                node_power_needed += node_ptr->power->min_watts;
	               	}
	        }
		if (cap_watts > alloc_power)
                	avail_power = cap_watts - alloc_power;
	        if ((alloc_power > cap_watts) || (node_power_needed > avail_power)) {
        	        // When CapWatts changes, we might need to lower nodes more
                	 // than the configured change rate specifications 
	                uint32_t red1 = 0, red2 = 0, node_num;
        	        if (alloc_power > cap_watts)
        	                red1 = alloc_power - cap_watts;
        	        if (node_power_needed > avail_power)
                	        red2 = node_power_needed - avail_power;
               		 red1 = MAX(red1, red2);
	                node_num = node_power_lower_cnt + node_power_same_cnt;
        	        if (node_num == 0)
                	        node_num = node_record_count;
               		 red1 /= node_num;
	                for (i = 0, node_ptr = node_record_table_ptr;
        	             i < node_record_count; i++, node_ptr++) {
                		if (!node_ptr->power || !node_ptr->power->new_cap_watts)
                                	continue;
	                        tmp_u32 = node_ptr->power->new_cap_watts -
        	                          node_ptr->power->min_watts;
               	        	tmp_u32 = MIN(tmp_u32, red1);
	                        node_ptr->power->new_cap_watts -= tmp_u32;
        	                alloc_power -= tmp_u32;
               		}
	                avail_power = cap_watts - alloc_power;
       		}
   	     	if (debug_flag & DEBUG_FLAG_POWER) {
            	    info("%s: distributing %u watts over %d nodes",
                 	    __func__, avail_power, node_power_raise_cnt);
		}
		if (node_power_raise_cnt) {
        	        ave_power = avail_power / node_power_raise_cnt;
                	for (i = 0, node_ptr = node_record_table_ptr;
	                     i < node_record_count; i++, node_ptr++) {
         	              	if (!node_ptr->power || (node_ptr->power->state != 1))
                	                continue;
                       		if (node_ptr->power->new_cap_watts)    // Already set 
                                continue;
	                        if (node_ptr->power->new_job_time >= recent) {
        	                        // Recent change in workload, do full reset 
                	                new_cap = ave_power;
                       		 } else {
                               	 // No recent change in workload, do partial
                                 // power cap reset (add up to increase_rate) 
                               	 tmp_u32 = node_ptr->power->max_watts -
                                          node_ptr->power->min_watts;
                               	 tmp_u32 = (tmp_u32 * increase_rate) / 100;
                               	 new_cap = node_ptr->power->cap_watts + tmp_u32;
                               	 new_cap = MIN(new_cap, ave_power);
                      		 }
                       	 	 node_ptr->power->new_cap_watts =
	                               	 MAX(new_cap, node_ptr->power->min_watts);
		                 node_ptr->power->new_cap_watts =
	                                MIN(node_ptr->power->new_cap_watts,
                                    node_ptr->power->max_watts);
        	                if (avail_power > node_ptr->power->new_cap_watts)
                	                avail_power -= node_ptr->power->new_cap_watts;
                       		else
                               	 avail_power = 0;
                       		 node_power_raise_cnt--;
	                        if (node_power_raise_cnt == 0)
        	                        break;  // No more nodes to modify 
                        	if (node_ptr->power->new_cap_watts != ave_power) {
                                	// Re-normalize 
                               		ave_power = avail_power / node_power_raise_cnt;
                       		 }
                    }
              }

		for(i=0, node_ptr = node_record_table_ptr; i < node_record_count; i++, node_ptr++) {


	//		get_host_power_info(node_record_table_ptr->name , set_caps);
			
			int x = 1, y = node_ptr->power->new_cap_watts;
			int pow = 10;
			while( y >= pow)
				pow *= 10;
			
			get_host_power_info(node_ptr->name , x*pow+y);
				
			
			node_ptr->power->cap_watts = node_ptr->power->new_cap_watts;

		
			debug("------------------------setting power caps  %d", node_ptr->power->cap_watts);


		}

	//	last_balance_time = time(NULL);
	}

	

}
/* _power_agent thread function gets called during the opac power plugin initialization and it runs only in the slurmctld and not in slurmd */
extern void *_power_agent(void *args)
{	
	debug("%s: %s", plugin_name, __func__);
	debug("info : intializing power agent thread function");
	
	populate_power_data();
	
	return NULL;
}



/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	pthread_attr_t attr;
	debug("%s: %s", plugin_name, __func__);
	
	if (!run_in_daemon("slurmctld")){
                _spawn_power_gathering_engine();
		return SLURM_SUCCESS;
	}

	slurm_mutex_lock(&thread_flag_mutex);
	
	if(power_thread) {
		debug2("Power thread already running, not starting another");
		slurm_mutex_unlock(&thread_flag_mutex);
		return SLURM_ERROR;
	}
	
	_load_config();
	
	slurm_attr_init(&attr);
	
	if (pthread_create(&power_thread, &attr, _power_agent, NULL))
		error("Unable to start power thread: %m");

	slurm_attr_destroy(&attr);
	slurm_mutex_unlock(&thread_flag_mutex);
	return SLURM_SUCCESS;
}

/* Terminate power thread */
static void stop_power_agent(void)
{
	stop_power = true;
	
}


/*
 * fini() is called when the plugin is unloaded. Free all memory.
 */
extern void fini(void)
{
	debug("%s: %s", plugin_name, __func__);
	if (power_thread){
	stop_power_agent();
	pthread_join(power_thread,NULL);
	power_thread=0;
	}
	
	return;
}

/* Read the configuration file */
extern void power_p_reconfig(void)
{
	debug("%s: %s", plugin_name, __func__);
	return;
}

/* Note that a suspended job has been resumed */
extern void power_p_job_resume(struct job_record *job_ptr)
{
	debug("%s: %s", plugin_name, __func__);
	return;
}

/* Note that a job has been allocated resources and is ready to start */
extern void power_p_job_start(struct job_record *job_ptr)
{
	debug("%s: %s", plugin_name, __func__);
//	int i;
//	uint64_t curr_watts;
	//struct node_record *node_r;
	
	//populate_power_data();

	
	//struct node_record *node_ptr;
	//curr_watts = node_record_table_ptr->power->current_watts;

	debug("%s, setting node new job\n", __func__);

	set_node_new_job(job_ptr, node_record_table_ptr);
	debug("%s, finished setting node new job\n", __func__);
	//get_host_power_info(node_record_table_ptr->name , set_caps);


}
