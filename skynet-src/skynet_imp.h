#ifndef SKYNET_IMP_H
#define SKYNET_IMP_H

struct skynet_config {
	int thread;                 /*tot thread*/
	int harbor;                 /*harbor id*/
	const char * daemon;        
	const char * module_path;   /*module dir*/
	const char * bootstrap;   
	const char * logger;        /*logfile path*/
	const char * logservice;
};

#define THREAD_WORKER 0             /*thread for module*/
#define THREAD_MAIN 1               /*thread for main*/
#define THREAD_SOCKET 2             /*thread for socket*/
#define THREAD_TIMER 3              /*thread for time*/
#define THREAD_MONITOR 4            /*thread for monitor*/

/*start skynet*/
void skynet_start(struct skynet_config * config);

#endif
