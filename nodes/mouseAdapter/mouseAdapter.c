#include <stdlib.h>
#include <stdio.h>
#include <unistd.h> /* close() */
#include <pthread.h>
#include <fcntl.h> // File control definitions
#include <linux/input.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include "brand.h"
#include "hiredis.h"

#define MOUSE_RELATIVE 1
#define MOUSE_ABSOLUTE 2

// Parameters read from supergraph, facilitates function definition
typedef struct graph_parameters_t {
    int samples_per_redis_stream;
    int sample_rate;
    char mouse_device[100];
	int max_samples;
} graph_parameters_t;


void initialize_redis(command_line_args_t *p);
void initialize_signals();
void handler_SIGINT(int exitStatus);
//void handler_SIGUSR1(int exitStatus);
void initialize_parameters(graph_parameters_t *p, redisContext *c);
void shutdown_process();

char NICKNAME[20] = "mouseAdapter";
redisReply *reply;
redisContext *redis_context;

int flag_SIGINT = 0;
int flag_SIGUSR1 = 0;

int16_t mouseData[6];  // (change in) X, Y position
int16_t mouseData_last[6];  // (change in) X, Y position
int mouseMode = 0;
pthread_t listenerThread;
pthread_t publisherThread;

int mouse_fd = -1;  // file descriptor for the mouse input
// mutex for mouseData
pthread_mutex_t mouseDataMutex = PTHREAD_MUTEX_INITIALIZER;

void * mouseListenerThread(void * thread_params) {
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);

	struct input_event ev;  // mouse input event
	int rd = 0;  // read status of mouse

	// wait for mouse input
	while(1) {

		rd = read(mouse_fd, &ev, sizeof(struct input_event));  // wait for input
		if (rd < (int) sizeof(struct input_event)) {
			perror("Mouse: error reading \n");
			exit(1);
		}
		pthread_mutex_lock(&mouseDataMutex);
		
		// What kind of event?
		if(ev.type == EV_REL) {  // mouse movements
			switch(ev.code) {
				case REL_X :  // change in X position
					mouseData[0] += ev.value;
					break;
				case REL_Y :  // change in Y position
					mouseData[1] += ev.value;
					break;
				case REL_WHEEL :  // change in wheel position
					mouseData[2] += ev.value;
					break;
			}
		} else if(ev.type == EV_KEY) {  // button presses
			switch(ev.code) {
				case BTN_LEFT :
					mouseData[3] = ev.value;
					break;
				case BTN_MIDDLE :
					mouseData[4] = ev.value;
					break;
				case BTN_RIGHT :
					mouseData[5] = ev.value;
					break;
			}
		}
		pthread_mutex_unlock(&mouseDataMutex);
	}
	return 0;
}


int main(int argc_main, char **argv_main) {
	
	int rc;

	printf("[%s] Parsing command line args...\n", NICKNAME);

	// Parse command line args
	command_line_args_t command_line_args;
	parse_command_line_args(argc_main, argv_main, &command_line_args);

	printf("[%s] Parsed command line args...\n", NICKNAME);

	// Parse nickname from args
    strcpy(NICKNAME, command_line_args.node_stream_name); 

	initialize_redis(&command_line_args);
	initialize_signals();

	// bring in parameters from the yaml setup file
	//yaml_parameters_t yaml_parameters = {};
	graph_parameters_t graph_parameters;
	initialize_parameters(&graph_parameters, redis_context);
	int16_t sampPerRedis = graph_parameters.samples_per_redis_stream;

	// array to keep track of system time
	struct timeval current_time;
	struct timespec current_timespec;

	// number of arguments etc for calls to redis
	int argc = 12; // number of arguments: "xadd mouse_vel maxlen ~ [maxlen] * timestamps [timestamps] samples [X Y] index_sample [index_sample]"
	size_t *argvlen = malloc(argc * sizeof(size_t)); // an array of the length of each argument put into Redis. This initializes the array

	int ind_xadd = 0; // xadd mouse_vel maxlen ~ [maxlen] *
	int ind_timestamps = ind_xadd + 6; // timestamps [timestamps]
	int ind_samples = ind_timestamps + 2; // samples [X Y] -- putting them in an array together rather than having a separate entry for each
	int ind_index_sample = ind_samples + 2; // index_sample [index_sample]
	
	// allocating memory for the actual data being passed
	int len = 16;  // maximum length of command entry
	char *argv[argc];

	// xadd mouse_vel maxlen ~ [maxlen] *
	for (int i = 0; i < ind_timestamps; i++) {
		argv[i] = malloc(len);
	} 

	// timestamps
	argv[ind_timestamps] = malloc(len);
	argv[ind_timestamps+1] = malloc(sizeof(struct timeval));
	argv[ind_samples]   = malloc(len);
	argv[ind_samples+1] = malloc(2 * sampPerRedis * sizeof(int16_t)); // number of samples * two inputs * float64 size
	argv[ind_index_sample] = malloc(len);
	argv[ind_index_sample+1] = malloc(sizeof(int32_t));

	// initialize sample array
	int16_t samples[3 * sampPerRedis];

	// initialize index for samples
	int32_t index_sample = 0;

	// populating the argv strings
	// start with the "xadd mouse_vel"
	argvlen[0] = sprintf(argv[0], "%s", "xadd"); // write the string "xadd" to the first position in argv, and put the length into argv
	argvlen[1] = sprintf(argv[1], "%s", "mouse_vel"); //same for cerebus adapter
	argvlen[2] = sprintf(argv[2], "%s", "maxlen");
	argvlen[3] = sprintf(argv[3], "%s", "~");
	argvlen[4] = sprintf(argv[4], "%d", graph_parameters.max_samples);
	argvlen[5] = sprintf(argv[5], "%s", "*");

	// and the samples array label
	argvlen[ind_timestamps] = sprintf(argv[ind_timestamps], "%s", "timestamps");
	argvlen[ind_timestamps+1] = sizeof(struct timeval);
	argvlen[ind_samples] = sprintf(argv[ind_samples], "%s", "samples"); // samples label
	argvlen[ind_samples+1] = sizeof(samples);
	argvlen[ind_index_sample] = sprintf(argv[ind_index_sample], "%s", "index");
	argvlen[ind_index_sample+1] = sizeof(ind_index_sample);
	
	// Mouse Initialization
	mouse_fd = open(graph_parameters.mouse_device, O_RDONLY);
	if (mouse_fd < 0) {
		printf("[%s] Error opening mouse. Status code %d.\n", NICKNAME, mouse_fd);
		return 1;
	}

	/* Spawn Mouse thread */
	printf("[%s] Starting mouse thread\n", NICKNAME);
	rc = pthread_create(&listenerThread, NULL, mouseListenerThread, NULL);
	if(rc) {
		printf("[%s] Mouse thread failed to initialize!!\n", NICKNAME);
	}

    int sample_rate = graph_parameters.sample_rate;
    long timer_period = 1000000000 / sample_rate;
    printf("[%s] Setting the timer to go off every %ld nanoseconds...\n",
        NICKNAME, timer_period);

    // get the initial time (for use in clock_nanosleep)
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);

	/* server infinite loop */
	while(1) {
		//pause();

		if (flag_SIGINT)
			shutdown_process();

        // update deadline
        deadline.tv_nsec += timer_period;
        if (deadline.tv_nsec >= 1000000000L) {
            // if ns field overflows, increment the seconds field
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }

        // sleep
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline,
            NULL);

		// read from the mouse
		pthread_mutex_lock(&mouseDataMutex);
		samples[0] = mouseData[0] - mouseData_last[0]; // x move
		samples[1] = mouseData[1] - mouseData_last[1]; // y move
		samples[2] = mouseData[3]; // left button press
		// update previous mouse state
		mouseData_last[0] = mouseData[0];
		mouseData_last[1] = mouseData[1];
		pthread_mutex_unlock(&mouseDataMutex);

		// read the current time into the array
		gettimeofday(&current_time, NULL);
		clock_gettime(CLOCK_MONOTONIC, &current_timespec);
		memcpy(&argv[ind_timestamps+1][0],
			&current_timespec, sizeof(struct timespec));

		// update argvlen[timestamps+1]
		argvlen[ind_timestamps+1] = sizeof(current_timespec);

		// copy each sample directly to the argv
		for(int i = 0; i < 3; i++) {
			memcpy(&argv[ind_samples + 1][i * sizeof(int16_t)], 
			&samples[i], sizeof(int16_t));
		}

		// update argvlen[samples+1]
		argvlen[ind_samples+1] = sizeof(samples);

		// write index of current sample into the array
		memcpy(&argv[ind_index_sample + 1][0], 
			&index_sample, sizeof(int32_t));

		// send everything to Redis
		freeReplyObject(redisCommandArgv(redis_context, argc,
			(const char**) argv, argvlen));

		index_sample++;

	}

	return 0;

}


void initialize_redis(command_line_args_t *p) {

	
	printf("[%s] Initializing Redis...\n", NICKNAME);

	//redis_context = redisConnect(redis_ip, atoi(redis_port));
	//redis_context = connect_to_redis_from_commandline_flags(argc, argv);
	redis_context = redisConnect(p->redis_host, p->redis_port); 
	if (redis_context->err) {
		printf("[%s] Redis connection error: %s\n", NICKNAME, redis_context->errstr);
		exit(1);
	}

	printf("[%s] Redis initialized.\n", NICKNAME);
		
}

void initialize_signals() {

	printf("[%s] Attempting to initialize signal handlers.\n", NICKNAME);

	signal(SIGINT, &handler_SIGINT);

	printf("[%s] Signal handlers installed.\n", NICKNAME);
}

//------------------------------------
// Handler functions
//------------------------------------

/*
void initialize_parameters(graph_parameters_t *p, char *yaml_path) {
	// create the strings to pull everything in from the yaml file
	char samples_per_redis_stream_string[16] = {0};
    char sample_rate_string[16] = {0};
	char mouse_device_string[100] = {0};
	char max_samples_string[16] = {0};

	// pull it in from the YAML
	load_YAML_variable_string(PROCESS, yaml_path, "samples_per_redis_stream",
		samples_per_redis_stream_string, 
		sizeof(samples_per_redis_stream_string));
	load_YAML_variable_string(PROCESS, yaml_path, "sample_rate",
		sample_rate_string, 
		sizeof(sample_rate_string));
	load_YAML_variable_string(PROCESS, yaml_path, "mouse_device",
		mouse_device_string, 
		sizeof(mouse_device_string));
	load_YAML_variable_string(PROCESS, yaml_path, "max_samples",
		max_samples_string, 
		sizeof(max_samples_string));

	// add it into yaml parameters struct
	p->samples_per_redis_stream = atoi(samples_per_redis_stream_string);
    p->sample_rate = atoi(sample_rate_string);
	strcpy(p->mouse_device, mouse_device_string);
	p->max_samples = atoi(max_samples_string);
}
*/

//------------------------------------------------------------------
// Initialize the parameters based on the supergraph. This reads from
// a valid supergraph structure and then populates the parameters struct
//------------------------------------------------------------------
void initialize_parameters(graph_parameters_t *p, redisContext *c) 
{
    // Initialize Supergraph_ID 
    char SUPERGRAPH_ID[] = "0";
    // Now fetch data from the supergraph and populate entries
    //redisReply *reply = NULL; bool bgsave_flag; int rediswritetime;
    const nx_json *supergraph_json = get_supergraph_json(c, reply, SUPERGRAPH_ID); 
    if (supergraph_json == NULL) {
        emit_status(c, NICKNAME, NODE_FATAL_ERROR, "No supergraph found for initialization. Aborting.");
        exit(1);
    }

	
    p->samples_per_redis_stream = get_parameter_int(supergraph_json, NICKNAME , "samples_per_redis_stream");
	p->sample_rate = get_parameter_int(supergraph_json, NICKNAME , "sample_rate");
    //get_parameter_string(supergraph_json   , NICKNAME , "mouse_device"    , &p->mouse_device);
	strcpy(p->mouse_device, get_parameter_string(supergraph_json, NICKNAME , "mouse_device"));
    p->max_samples = get_parameter_int(supergraph_json, NICKNAME , "max_samples");
	
	 
	printf("[%s] Parameters have been loaded.\n", NICKNAME);
    printf("[%s] samples_per_redis_stream: %d\n", NICKNAME, p->samples_per_redis_stream);
    printf("[%s] sample_rate: %d\n", NICKNAME, p->sample_rate);
	printf("[%s] mouse_device: %s\n", NICKNAME, p->mouse_device);
    printf("[%s] max_samples: %d\n", NICKNAME, p->max_samples);

    // Free memory, since all relevant information has been transfered to the parameter struct at this point
    //nx_json_free(supergraph_json);	
}

void shutdown_process() {

	printf("[%s] SIGINT received. Shutting down.\n", NICKNAME);

	printf("[%s] Setting scheduler back to baseline.\n", NICKNAME);
	const struct sched_param sched= {.sched_priority = 0};
	sched_setscheduler(0, SCHED_OTHER, &sched);

	printf("[%s] Shutting down redis.\n", NICKNAME);

	redisFree(redis_context);

	printf("[%s] Exiting.\n", NICKNAME);
	
	exit(0);
}

//------------------------------------
// Handler functions
//------------------------------------

void handler_SIGINT(int exitStatus) {
	flag_SIGINT++;
}

