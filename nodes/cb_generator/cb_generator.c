
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <netinet/udp.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h> /* close() */
#include <pthread.h>
#include <fcntl.h> // File control definitions
#include <linux/input.h>
#include <signal.h>
#include <string.h>
#include <termios.h> 
#include <sys/time.h>
#include <sys/ioctl.h>
#include "brand.h"
#include "hiredis.h"
#include <arpa/inet.h>

// Parameters read from supergraph, facilitates function definition
typedef struct graph_parameters_t {
    char broadcast_ip[20];
    int broadcast_port;
    char broadcast_device[20];
    int broadcast_rate;
    int sampling_frequency;
    int num_channels;
    uint32_t initial_timestamp;
    int bool_log_time;
    int bool_serial_clk;
    char input_stream_name[30];
    int sample_buffering;
} graph_parameters_t;

typedef struct cerebus_packet_t {
    uint32_t time;
    uint16_t chid;
    uint8_t type;
    uint8_t dlen;
    //uint16_t data[96];
} cerebus_packet_t;

void initialize_signals();
void handler_SIGALRM(int signum); // done
void handler_SIGINT(int signum);
void handler_SIGUSR1(int signum);
void initialize_parameters(graph_parameters_t *p, redisContext *c);
struct timespec get_graph_load_timespec(redisContext *c);
void shutdown_process();

uint32_t parse_ip_str(char *ip_str);

int  initialize_broadcast_socket();
//void initialize_realtime(char *yaml_path); // done
//int  initialize_ramp(char **, int, char *yaml_path);
//int  initialize_buffer(char **, int, char *yaml_path);

// utilities for serial port comm (control clk output)
int serialport_init(const char* serialport, int baud);
int serialport_close( int fd );
int serialport_flush(int fd);
int serialport_writebyte( int fd, uint8_t b);
void error(char* msg, int ex);

char NICKNAME[20] = "cb_generator";
redisReply *reply;
redisContext *redis_context;

int one = 1, zero = 0;

int flag_SIGINT  = 0;
int flag_SIGALRM = 0;
int flag_SIGUSR1 = 0;

//char log_filename [30];
//FILE *logfile;

uint32_t loop_count = 0;

int main(int argc_main, char **argv_main) {  

	printf("[%s] Parsing command line args...\n", NICKNAME);

	// Parse command line args and init redis
	redis_context = parse_command_line_args_init_redis(argc_main, argv_main, NICKNAME);

	initialize_signals();

	// bring in parameters from the supergraph
	graph_parameters_t graph_parameters;
	initialize_parameters(&graph_parameters, redis_context);

    int fd = initialize_broadcast_socket(&graph_parameters);

    int num_channels            = graph_parameters.num_channels;
    int sampling_frequency      = graph_parameters.sampling_frequency;
    int broadcast_rate          = graph_parameters.broadcast_rate;
    uint32_t initial_timestamp  = graph_parameters.initial_timestamp;
    int bool_log_time           = graph_parameters.bool_log_time;        


    printf("[%s] num_channels: %d.\n", NICKNAME, num_channels);
    printf("[%s] sampling_frequency: %d.\n", NICKNAME, sampling_frequency);
    printf("[%s] broadcast_rate: %d.\n", NICKNAME, broadcast_rate);
    printf("[%s] initial_timestamp: %u.\n", NICKNAME, initial_timestamp);

    int cerebus_packets_per_SIGALRM = sampling_frequency / broadcast_rate;

    printf("[%s] cerebus_packets_per_SIGALRM: %d.\n", NICKNAME, cerebus_packets_per_SIGALRM);

    /*    
    printf("[%s] Broadcasting %d packets per %d microseconds...\n", 
                NICKNAME,
                num_cerebus_packets_per_signal, broadcast_rate);
    */

    // Initialize serial port
    int fd_serial = -1;
    char serialport[20] = "/dev/ttyACM0"; // TODO: add to yaml file
    int baudrate = 115200;                // TODO: add to yaml file
    int rc_serial;
    int bool_serial_clk = graph_parameters.bool_serial_clk;

    if (bool_serial_clk)
    {
        fd_serial = serialport_init(serialport, baudrate);
        if( fd_serial==-1 ) error("Couldn't open port", 1);
        printf("[%s] opened port %s\n", NICKNAME,serialport);
        serialport_flush(fd_serial);
    }

    char stream_name[30] = {0};     
    strcpy(stream_name, graph_parameters.input_stream_name);
    
    uint32_t ts = initial_timestamp;

    struct timeval current_time;
    struct timespec current_timespec;

    long timer_period = 1 * 1000000000 / broadcast_rate;
    printf("[%s] Setting the timer to go off every %ld nanoseconds...\n",
        NICKNAME, timer_period);

    // get the initial time (for use in clock_nanosleep)
    struct timespec deadline;

    // TODO: get initial deadline for synching processes
    int custom_init_deadline = 0;
    if(custom_init_deadline)
    {     	    
        struct timespec custom_deadline = get_graph_load_timespec(redis_context);
        deadline.tv_sec = custom_deadline.tv_sec;
        deadline.tv_sec += 10;
        deadline.tv_nsec = custom_deadline.tv_nsec;
    }
    else
    {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
    }

    printf("[%s] Initial deadline seconds: %ld\n",
        NICKNAME, deadline.tv_sec);
    printf("[%s] Initial deadline nanoseconds: %ld\n",
        NICKNAME, deadline.tv_nsec);

    char last_redis_id [30];
    strcpy(last_redis_id, "0-0");
    char redis_string[256] = {0};

    uint32_t buffer_write_ind = 0;
    // TODO: make a yaml parameter
    int sample_buffering = graph_parameters.sample_buffering;
    uint32_t samples_received = 0;

    // preallocate cerebus packet headers
    /*
    cerebus_packet_t cerebus_headers [30];
    for (int i=0; i<30; i++)
    {
        cerebus_headers[i].time = 0;
        cerebus_headers[i].chid = 0;
        cerebus_headers[i].dlen = num_channels / 2;
        cerebus_headers[i].type = 6;
    }
    */
    // pre-allocate cerebus packet header struct
    cerebus_packet_t cb_packet;
    cb_packet.time = 0;
    cb_packet.chid = 0;
    cb_packet.dlen = num_channels / 2;
    cb_packet.type = 5;

    char *redis_data;

    //////////////////////////////////
    // timing logging through redis
    //////////////////////////////////

    // number of arguments etc for calls to redis
	int argc = 5; // number of arguments: "xadd NICKNAME * timestamps [timestamps]" **(index_sample [index_sample])
	size_t *argvlen = malloc(argc * sizeof(size_t)); // an array of the length of each argument put into Redis. This initializes the array
    int ind_xadd = 0; // xadd NICKNAME *
    int ind_timestamps = ind_xadd + 3; // timestamps [timestamps]

	// allocating memory for the actual data being passed
	int len = 16;  // maximum length of command entry
	char *argv[argc];

	// xadd NICKNAME *
	for (int i = 0; i < ind_timestamps; i++) {
		argv[i] = malloc(len);
	} 

	// timestamps
	argv[ind_timestamps] = malloc(len);
	argv[ind_timestamps+1] = malloc(sizeof(struct timeval));

	// populating the argv strings
	// start with the "xadd NICKNAME"
	argvlen[0] = sprintf(argv[0], "%s", "xadd"); // write the string "xadd" to the first position in argv, and put the length into argv
	argvlen[1] = sprintf(argv[1], "%s", NICKNAME); //stream name
	argvlen[2] = sprintf(argv[2], "%s", "*");

	// and the samples array label
	argvlen[ind_timestamps] = sprintf(argv[ind_timestamps], "%s", "timestamps");
	argvlen[ind_timestamps+1] = sizeof(struct timeval);

    printf("[%s] Starting main loop...\n", NICKNAME);

    while (1) 
    {
        if (flag_SIGINT) 
            shutdown_process();
       
        // Wait until at least [sample_buffering] samples have been received
        if(samples_received <= sample_buffering)
        {          
            freeReplyObject(reply); 
            // Check how many samples have been received
            sprintf(redis_string, "xlen %s", stream_name);
            reply = redisCommand(redis_context, redis_string);
            // If no samples yet or invalid reply
            if (reply == NULL || reply->type != REDIS_REPLY_INTEGER)
            {
                // Do nothing
            }
            else
            {
                samples_received = reply -> integer;
                // update deadline
                if(!custom_init_deadline) {
                    clock_gettime(CLOCK_MONOTONIC, &deadline);
                }
            }
        }
        else
        {
            freeReplyObject(reply); 
            // Read new samples from redis stream (should be from a couple of ms back)
            sprintf(redis_string, "xread count 1 streams %s %s", stream_name, last_redis_id);
            reply = redisCommand(redis_context, redis_string);
            if (reply == NULL || reply->type != REDIS_REPLY_ARRAY)
            {
                // The above redis call could maybe be blocking
                continue;
            }
            else
            {
                // The xread value is nested:
                // dim0 [0] The first stream (threshold_values)
                // dim1 [1] Stream data
                // dim2 [0+] The stream samples we're getting data from
                // dim3 [0] The redis timestamp
                // dim3 [1] The data content from the stream
                // dim4 [13] The continuous data content from the stream

                // Save timestamp/id of last redis sample
                strcpy(last_redis_id, reply->element[0]->element[1]->element[0]->element[0]->str);  
                // Copy redis data  
                redis_data = reply->element[0]->element[1]->element[0]->element[1]->element[13]->str;

                int num_cb_data_packets = 0;
                int udp_packet_size = 0;
                uint32_t buffer_ind = 0;            

                while(num_cb_data_packets < cerebus_packets_per_SIGALRM) {

                    // update Cerebus packet timestamp
                    cb_packet.time = ts;
                    ts++;
                    // The cerebus packet definitions use dlen to determine the number of 4 bytes 
                    int cb_packet_size = sizeof(cerebus_packet_t) + (cb_packet.dlen * 4);
                    
                    // Write header bytes
                    write(fd, &cb_packet, sizeof(cerebus_packet_t));
                    udp_packet_size += sizeof(cerebus_packet_t);
                    // Write data bytes
                    write(fd, &redis_data[buffer_ind], num_channels*2);
                    udp_packet_size += num_channels*2;
                    buffer_ind      += num_channels*2;

                    /*
                    // Prevent index out of bounds problems
                    if (buffer_ind >= buffer_size) {
                        buffer_ind = 0;
                    }
                    */

                    // Having read the packet, we then ask if writing the next packet
                    // will take us over. If yes, then uncork/cork again.
                    int next_cb_packet_size = sizeof(cerebus_packet_t) + (cb_packet.dlen*4);

                    if (udp_packet_size + next_cb_packet_size >= 1472) {
                        setsockopt(fd, IPPROTO_UDP, UDP_CORK, &zero, sizeof(zero));
                        setsockopt(fd, IPPROTO_UDP, UDP_CORK, &one, sizeof(one));
                        udp_packet_size = 0;
                    }

                    /*
                    if (cb_packet.type == 5) {
                        num_cb_data_packets++;
                    }
                    */
                    num_cb_data_packets++;

                }
                
                // Now that we've sent all of the packets we're going to send, uncork and then cork
                setsockopt(fd, IPPROTO_UDP, UDP_CORK, &zero, sizeof(zero));
                setsockopt(fd, IPPROTO_UDP, UDP_CORK, &one, sizeof(one));

                loop_count++;

                //freeReplyObject(reply);

                // Log current time to redis
                if (bool_log_time)
                {
                    freeReplyObject(reply); 

                    // Get current time
                    gettimeofday(&current_time, NULL);
                    clock_gettime(CLOCK_MONOTONIC, &current_timespec);
                    memcpy(&argv[ind_timestamps+1][0],
                        &current_timespec, sizeof(struct timespec));
                    
                    // Update argvlen[timestamps+1]
                    argvlen[ind_timestamps+1] = sizeof(current_timespec);

                    // Send to redis
                    reply = redisCommandArgv(redis_context, argc,
                        (const char**) argv, argvlen);
                } 

                if (bool_serial_clk)
                {
                    // Send serial message to Arduino (or other peripheral peripherial) to trigger clock pulse
                    rc_serial = serialport_writebyte(fd_serial, (uint8_t)2);
                    if(rc_serial==-1) error("error writing",0);
                }                          
            }
        }

        // Update clock_nanosleep deadline
        deadline.tv_nsec += timer_period;
        if (deadline.tv_nsec >= 1000000000L) {
            // If ns field overflows, increment the seconds field
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }

        // Sleep until next deadline
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);

        // Wait until [sample_buffering] samples have already been buffered before sending packets
        if (samples_received > sample_buffering)
        {


            
        }         
    }

    //free(buffer);
    return 0;
}

int initialize_broadcast_socket(graph_parameters_t *p) {

    printf("[%s] Initializing socket...\n", NICKNAME);
    
    // Create a socket. I think IPPROTO_UDP is needed for broadcasting
   	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP ); 
    if (fd == 0) {
        perror("[cb_generator] socket failed"); 
        exit(EXIT_FAILURE); 
    }

    // Set broadcast socket option
    int broadcastPermission = 1;
    if (setsockopt(fd,SOL_SOCKET,SO_BROADCAST , (void *) &broadcastPermission, sizeof(broadcastPermission)) < 0) {
        perror("[cb_generator] socket permission failure"); 
        exit(EXIT_FAILURE); 
    }

    // Load the broadcast IP
    char broadcast_ip_string[INET_ADDRSTRLEN];
    strcpy(broadcast_ip_string, p->broadcast_ip);
    uint32_t broadcast_ip = parse_ip_str(broadcast_ip_string);

    // Load the broadcast port
    int broadcast_port = p->broadcast_port;
    
    printf("[%s] Emitting on IP: %s, port: %d.\n", NICKNAME, broadcast_ip_string, broadcast_port);

    // Bind socket to a specific interface
    char interface[20] = {0};
    strcpy(interface, p->broadcast_device);
    printf("[%s] Bind socket to device: %s.\n", NICKNAME, interface);
    
    int so = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface,
        sizeof(interface));
    //int so = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, "eno1", strlen("eno1"));
    if (so < 0) {
        perror("[generator] interface binding error");
    }

    /*
    // Now configure the socket
    struct sockaddr_in srcaddr;
    memset(&srcaddr,0,sizeof(srcaddr));
    srcaddr.sin_family      = AF_INET;
    //srcaddr.sin_addr.s_addr = "127.0.0.1";
    srcaddr.sin_port        = htons(51001);

    if (bind(fd, (struct sockaddr *) &srcaddr, sizeof(srcaddr)) < 0) {
        perror("bind");
    }
    */

    // Configure the socket
    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = broadcast_ip;
    addr.sin_port        = htons(broadcast_port);

    // Start corking right away
	setsockopt(fd, IPPROTO_UDP, UDP_CORK, &one, sizeof(one)); // CORK

    // Connect here instead of using sentto because it's faster; kernel doesn't need to make
    // the necessary checks because it already has a valid file descriptor for the socket
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("[cb_generator] connect error");
        exit(EXIT_FAILURE);
    }

    return fd;
}

// int initialize_buffer(char **buffer, int numChannels, char *yaml_path) {

//     /*
//     char use_ramp_string[16] = {0};
//     load_YAML_variable_string(NICKNAME, yaml_path, "use_ramp", use_ramp_string, sizeof(use_ramp_string));

//     if (strcmp(use_ramp_string, "True") == 0) {
//         printf("[%s] Generating data from a ramp.\n", NICKNAME);
//         return initialize_ramp(buffer, numChannels, yaml_path);
//     } else {
//         printf("[%s] Generating data from a file.\n", NICKNAME);
//         //return initialize_from_file(buffer, numChannels);
//         return initialize_ramp(buffer, numChannels, yaml_path);
//     }
//     */
//     int buffer_size = (sizeof(cerebus_packet_t) + numChannels*2) * 30 * 5;
//     *buffer = malloc(buffer_size);

//     printf("[%s] Memory allocated for data buffer.\n", NICKNAME);

//     return buffer_size;

// }

// int initialize_ramp(char  **buffer, int num_channels, char *yaml_path) {

//     printf("[%s] Initializing ramp function...\n", NICKNAME);

//     char ramp_max_string[16] = {0};
//     load_YAML_variable_string(NICKNAME, yaml_path, "ramp_max", ramp_max_string, sizeof(ramp_max_string));
//     int ramp_max = atoi(ramp_max_string);
//     printf("[%s] Ramp goes from 0 to %d.\n", NICKNAME, ramp_max);

//     int buffer_size = (sizeof(cerebus_packet_t) + num_channels*2) * ramp_max;
// //int cb_packet_size = sizeof(cerebus_packet_t);// + (cb_packet->dlen * 4);
//     *buffer = malloc(buffer_size);

//     printf("[%s] Memory allocated for ramp.\n", NICKNAME);



//     uint16_t data = 0;

//     uint32_t current_byte = 0;

//     for (int i = 0; i < ramp_max; i++) {
        
//         cerebus_packet_t cerebus_packet = {0};
//         cerebus_packet.time = 0;
//         cerebus_packet.chid = 0;
//         cerebus_packet.dlen = num_channels / 2;
//         cerebus_packet.type = 5;

//         /*
//         for (int j = 0; j < num_channels; j++) {
//             cerebus_packet.data[j] = i;
//         }
//         data++;
//         */

//         memcpy(&(*buffer)[current_byte], &cerebus_packet, sizeof(cerebus_packet_t));
        
//         current_byte += sizeof(cerebus_packet_t);

//         for (int j = 0; j < num_channels; j++) {
//             memcpy(&(*buffer)[current_byte], &data, sizeof(data));
//             current_byte += sizeof(data);
//         }
        
//         data++;

//         //printf("[%s] buffer ind: %d.\n", NICKNAME, current_byte);
//     }

//     printf("[%s] Buffer filled with ramp data.\n", NICKNAME);

//     return buffer_size;

// }

//------------------------------------------------------------------
// Initialize the parameters based on the supergraph. This reads from
// a valid supergraph structure and then populates the parameters struct
//------------------------------------------------------------------
void initialize_parameters(graph_parameters_t *p, redisContext *c) 
{
    // Initialize Supergraph_ID 
    char SUPERGRAPH_ID[] = "0";
    // Now fetch data from the supergraph and populate entries
    const nx_json *supergraph_json = get_supergraph_json(c, reply, SUPERGRAPH_ID); 
    if (supergraph_json == NULL) {
        emit_status(c, NICKNAME, NODE_FATAL_ERROR, "No supergraph found for initialization. Aborting.");
        exit(1);
    }

    strcpy(p->broadcast_ip, get_parameter_string(supergraph_json, NICKNAME , "broadcast_ip"));
    p->broadcast_port = get_parameter_int(supergraph_json, NICKNAME , "broadcast_port");
    strcpy(p->broadcast_device, get_parameter_string(supergraph_json, NICKNAME , "broadcast_device"));
    p->broadcast_rate = get_parameter_int(supergraph_json, NICKNAME , "broadcast_rate");
    p->sampling_frequency = get_parameter_int(supergraph_json, NICKNAME , "sampling_frequency");
    p->num_channels = get_parameter_int(supergraph_json, NICKNAME , "num_channels");
    p->initial_timestamp = get_parameter_int(supergraph_json, NICKNAME , "initial_timestamp");
    p->bool_log_time = get_parameter_int(supergraph_json, NICKNAME , "bool_log_time");
    p->bool_serial_clk = get_parameter_int(supergraph_json, NICKNAME , "bool_serial_clk");
    strcpy(p->input_stream_name, get_parameter_string(supergraph_json, NICKNAME , "input_stream_name"));
    p->sample_buffering = get_parameter_int(supergraph_json, NICKNAME , "sample_buffering");

    // Free memory, since all relevant information has been transfered to the parameter struct at this point
    //nx_json_free(supergraph_json);	
}

struct timespec get_graph_load_timespec(redisContext *c) 
{
    // Initialize Supergraph_ID 
    char SUPERGRAPH_ID[] = "0";
    // Now fetch data from the supergraph and populate entries
    const nx_json *supergraph_json = get_supergraph_json(c, reply, SUPERGRAPH_ID); 
    if (supergraph_json == NULL) {
        emit_status(c, NICKNAME, NODE_FATAL_ERROR, "No supergraph found for initialization. Aborting.");
        exit(1);
    }

    struct timespec graph_loaded_timespec;

    unsigned long graph_loaded_ts = get_graph_load_ts_long(supergraph_json);
    printf("[%s] Graph loaded ts: %lu nanoseconds.\n",
        NICKNAME, graph_loaded_ts);

    graph_loaded_timespec.tv_sec = graph_loaded_ts/1000000000L;
    graph_loaded_timespec.tv_nsec = graph_loaded_ts - graph_loaded_timespec.tv_sec*1000000000L;

    return graph_loaded_timespec;
}

uint32_t parse_ip_str(char *ip_str) {
    unsigned char buf[sizeof(struct in_addr)];
    int domain, s;

    domain = AF_INET;
    s = inet_pton(domain, ip_str, buf);
    if (s <= 0) {
        if (s == 0) {
            fprintf(stderr, "[%s] Invalid IP address: '%s'\n", NICKNAME, ip_str);
        } else {
            perror("inet_pton");
        }
        fprintf(stderr, "[%s] Reverting to 255.255.255.255\n", NICKNAME);
        s = inet_pton(domain, "255.255.255.255", buf);
    }

    uint32_t ip_num;
    memcpy(&ip_num, buf, 4);

    return ip_num;
}

/*
// Do we want the system to be realtime?  Setting the Scheduler to be real-time, priority 80
void initialize_realtime(char *yaml_path) {

    char sched_fifo_string[16] = {0};
    load_YAML_variable_string(NICKNAME, yaml_path, "sched_fifo", sched_fifo_string, sizeof(sched_fifo_string));


    if (strcmp(sched_fifo_string, "True") != 0) {
        return;
    }

    printf("[%s] Setting Real-time scheduler!\n", NICKNAME);

    baseline_scheduler_priority = sched_getscheduler(0);

    struct sched_param sched= {.sched_priority = 80};
    if(sched_setscheduler(0, SCHED_FIFO, &sched) < 0) {
        printf("[%s] ERROR SCHED_FIFO SCHEDULER\n", NICKNAME);
    }

}
*/

void shutdown_process() {

	printf("[%s] SIGINT received. Shutting down.\n", NICKNAME);

    printf("[%s] Total 1ms samples sent: %d.\n", NICKNAME, loop_count);

    //fclose(logfile);

	printf("[%s] Setting scheduler back to baseline.\n", NICKNAME);
	const struct sched_param sched= {.sched_priority = 0};
	sched_setscheduler(0, SCHED_OTHER, &sched);

	printf("[%s] Shutting down redis.\n", NICKNAME);

    freeReplyObject(reply); 
	redisFree(redis_context);

	printf("[%s] Exiting.\n", NICKNAME);
	
	exit(0);
}

void initialize_signals() {

    printf("[%s] Attempting to initialize signal handlers.\n", NICKNAME);

    signal(SIGINT, &handler_SIGINT);
    //signal(SIGALRM, &handler_SIGALRM);
    //signal(SIGUSR1, &handler_SIGUSR1);

    printf("[%s] Signal handlers installed.\n", NICKNAME);
}

//------------------------------------
// Handler functions
//------------------------------------

void handler_SIGINT(int signum) {
    flag_SIGINT++;
}

void handler_SIGALRM(int signum) {
    flag_SIGALRM++;
}

void handler_SIGUSR1(int signum) {
	flag_SIGUSR1++;
}

//------------------------------------
// Serial port functions
// https://github.com/todbot/arduino-serial
//------------------------------------

// takes the string name of the serial port (e.g. "/dev/tty.usbserial","COM1")
// and a baud rate (bps) and connects to that port at that speed and 8N1.
// opens the port in fully raw mode so you can send binary data.
// returns valid fd, or -1 on error
int serialport_init(const char* serialport, int baud)
{
    struct termios toptions;
    int fd;
    
    //fd = open(serialport, O_RDWR | O_NOCTTY | O_NDELAY);
    fd = open(serialport, O_RDWR | O_NONBLOCK );
    
    if (fd == -1)  {
        perror("serialport_init: Unable to open port ");
        return -1;
    }
    
    //int iflags = TIOCM_DTR;
    //ioctl(fd, TIOCMBIS, &iflags);     // turn on DTR
    //ioctl(fd, TIOCMBIC, &iflags);    // turn off DTR

    if (tcgetattr(fd, &toptions) < 0) {
        perror("serialport_init: Couldn't get term attributes");
        return -1;
    }
    speed_t brate = baud; // let you override switch below if needed
    switch(baud) {
    case 4800:   brate=B4800;   break;
    case 9600:   brate=B9600;   break;
#ifdef B14400
    case 14400:  brate=B14400;  break;
#endif
    case 19200:  brate=B19200;  break;
#ifdef B28800
    case 28800:  brate=B28800;  break;
#endif
    case 38400:  brate=B38400;  break;
    case 57600:  brate=B57600;  break;
    case 115200: brate=B115200; break;
    }
    cfsetispeed(&toptions, brate);
    cfsetospeed(&toptions, brate);

    // 8N1
    toptions.c_cflag &= ~PARENB;
    toptions.c_cflag &= ~CSTOPB;
    toptions.c_cflag &= ~CSIZE;
    toptions.c_cflag |= CS8;
    // no flow control
    toptions.c_cflag &= ~CRTSCTS;

    //toptions.c_cflag &= ~HUPCL; // disable hang-up-on-close to avoid reset

    toptions.c_cflag |= CREAD | CLOCAL;  // turn on READ & ignore ctrl lines
    toptions.c_iflag &= ~(IXON | IXOFF | IXANY); // turn off s/w flow ctrl

    toptions.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // make raw
    toptions.c_oflag &= ~OPOST; // make raw

    // see: http://unixwiz.net/techtips/termios-vmin-vtime.html
    toptions.c_cc[VMIN]  = 0;
    toptions.c_cc[VTIME] = 0;
    //toptions.c_cc[VTIME] = 20;
    
    tcsetattr(fd, TCSANOW, &toptions);
    if( tcsetattr(fd, TCSAFLUSH, &toptions) < 0) {
        perror("init_serialport: Couldn't set term attributes");
        return -1;
    }

    return fd;
}

int serialport_close( int fd )
{
    return close( fd );
}

int serialport_flush(int fd)
{
    sleep(2); //required to make flush work, for some reason
    return tcflush(fd, TCIOFLUSH);
}

int serialport_writebyte( int fd, uint8_t b)
{
    int n = write(fd,&b,1);
    if( n!=1)
        return -1;
    return 0;
}

void error(char* msg, int ex)
{
    fprintf(stderr, "%s\n",msg);
    if (ex) exit(EXIT_FAILURE);
}