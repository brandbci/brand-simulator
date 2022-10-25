/*

The goal of this program is to emit ramp data at a specified data rate, emulating a 
Blackrock cerebus system. Rather than producing artifical neural data, we instead
produce "ramp" data, used for debugging

The critical variables are as follows:
    int num_channels            = 256;    The number of neurons we are emulating
    int sampling_frequency      = 30000;  The modeled sampling rate of neural data
    int broadcast_rate          = 1000;   The rate (in ms) we are transmitting data
    int ramp_max                = 1000;   The maximum ramp value used for debugging

The main part of this function is designed to populate the structure of a sendmmsg().
This function allows the user to send multiple messages at the same time, minimizing
the number of times that we need to switch between user and kernel space.

Hence, we are going to populate a number of UDP packets. The payload for the UDP packets
point to array entries of the buffer.

The function work as follows:

while true:

    Wait until SIGALRM fires

    if SIGALRM has fired:

        Set the pointers in the sendmmsg() array to the correct aspect of memory

        sendmmsg()

David Brandman, September 2022

*/


#define _GNU_SOURCE // Needed for sendmmsg
#include <sys/socket.h> 
#include <netinet/in.h> 
#include <netinet/udp.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <linux/input.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <time.h>
#include <sys/uio.h>

typedef struct cerebus_packet_header_t {
    uint32_t time;
    uint16_t chid;
    uint8_t type;
    uint8_t dlen;
} cerebus_packet_header_t;

void initialize_signals();
void initialize_alarm(long timer_period);
int  initialize_broadcast_socket();
uint32_t parse_ip_str(char *ip_str);
int  initialize_buffer(char **, int, int); 

void handler_SIGINT(int signum);
void handler_SIGALRM(int signum);

int flag_SIGINT  = 0;
int flag_SIGALRM = 0;

char NICKNAME[] = "EMIT";

int main(int argc_main, char **argv_main) {  

    char *buffer;
    int num_channels            = 96;
    int sampling_frequency      = 30000; 
    int broadcast_rate          = 1000;
    uint32_t initial_timestamp  = 0; 
    int ramp_max                = 1000; 

    int cerebus_packets_per_SIGALRM = sampling_frequency / broadcast_rate;
    int cb_packet_size = sizeof(cerebus_packet_header_t) + num_channels * 2;
    int num_cb_packets_per_udp = 1472 / cb_packet_size;

    printf("[%s] num_channels: %d.\n", NICKNAME, num_channels);
    printf("[%s] sampling_frequency: %d.\n", NICKNAME, sampling_frequency);
    printf("[%s] broadcast_rate: %d.\n", NICKNAME, broadcast_rate);
    printf("[%s] initial_timestamp: %u.\n", NICKNAME, initial_timestamp);
    printf("[%s] cerebus_packets_per_SIGALRM: %d.\n", NICKNAME, cerebus_packets_per_SIGALRM);
    printf("[%s] cerebus packet size: %d.\n", NICKNAME, cb_packet_size);
    printf("[%s] cerebus packets per UDP: %d.\n", NICKNAME, num_cb_packets_per_udp);

	initialize_signals();

    int buffer_size = initialize_buffer(&buffer, num_channels, ramp_max);

    int fd = initialize_broadcast_socket();
    
    //------------------------------------
    // Setting up the timer
    //------------------------------------


    long timer_period = 1 * 1000000000 / broadcast_rate;
    initialize_alarm(timer_period);

    //------------------------------------
    // Pre-allocate cerebus packet header struct
    //------------------------------------

    cerebus_packet_header_t cb_packet_header;
    cb_packet_header.time = 0;
    cb_packet_header.chid = 0;
    cb_packet_header.dlen = num_channels / 2;
    cb_packet_header.type = 5;

    // ---------------------------------------------
    // Pre-calculate the number of cb packets to be sent per UDP packets
    // ---------------------------------------------
    
    // Let C be the num_cb_packets_per_udp. This segment code is:
    // cb_per_udp = [C + C + C + .. + leftover], and sum(cb_per_udp) = cerebus_packets_per_SIGLARM

    int cb_per_udp[100] = {0}; // The number of CB packets per UDP packet, as an array
    int num_udp_to_send = 0; // The number of indices of the above array
    int n = 0; // The number per entry
    int n_total = 0; // The total number. Should add up to cerebus_packets_per_SIGARLM

    
    while (n_total < cerebus_packets_per_SIGALRM) {

        if (n_total + num_cb_packets_per_udp < cerebus_packets_per_SIGALRM) {
            n = num_cb_packets_per_udp;
        } else {
            n = cerebus_packets_per_SIGALRM - n_total;
        }

        cb_per_udp[num_udp_to_send] = n;
        printf("cb_per_udp[%d] = %d\n", num_udp_to_send, n);

        n_total += n;
        num_udp_to_send++;

    }
    
    // The goal of this segment of code is to create an array of iovec_cb_packets
    // Each iovec_cb_packets entry contains a reference to the CB packet header
    // as well as the payload (from the buffer). Note iov_base is populated from
    // the buffer in the main loop
    
    // The struct msghdr contains an array of contiguous iovec_cb_packets
    // according to the number of CB packets per UDP packet
    // Finally, the mmsghdr (output) contains an array of references to msghdr 

    // Create the iovec structs for output
    struct iovec iovec_cb_packets[cerebus_packets_per_SIGALRM * 2];
    for (int i = 0; i < cerebus_packets_per_SIGALRM * 2; i+=2) {

        iovec_cb_packets[i  ].iov_base = &cb_packet_header;
        iovec_cb_packets[i  ].iov_len  = sizeof(cerebus_packet_header_t);
        iovec_cb_packets[i+1].iov_len  = num_channels*2;

        // Conspicuously absent: the buffered neural data. 
        // This is populated this in the main() loop.
        // iovec_cb_packets[i+1].iov_base = &buffer[buffer_ind];

    }

    struct msghdr message_header[num_udp_to_send];
    struct mmsghdr output[num_udp_to_send];
    memset(&message_header, 0, sizeof(struct msghdr)  * num_udp_to_send);
    memset(&output,         0, sizeof(struct mmsghdr) * num_udp_to_send);

    for (int i = 0; i < num_udp_to_send; i++) {

        int this_cb_per_udp = cb_per_udp[i];
        message_header[i].msg_iov    = &iovec_cb_packets[2*this_cb_per_udp];
        message_header[i].msg_iovlen = 2*this_cb_per_udp;

        output[i].msg_hdr = message_header[i];
        output[i].msg_len = this_cb_per_udp;

    }
    
    //-------------------------------------------------
    printf("[%s] Starting main loop...\n", NICKNAME);
    //-------------------------------------------------

    int buffer_ind = 0;
    while (1) 
    {
        // Stop working if we get a SIGINT
        if (flag_SIGINT) 
           exit(1);

        if (flag_SIGALRM) {

            flag_SIGALRM--;

            // This part assigns pointers into the binary string buffer
            // to populate the iov_base content for the io vector

            for(int i = 0; i < cerebus_packets_per_SIGALRM * 2; i+=2) {

                iovec_cb_packets[i+1].iov_base = &buffer[buffer_ind];
                buffer_ind                    += num_channels*2;
                
                // Prevent index out of bounds problems
                if (buffer_ind >= buffer_size) {
                    buffer_ind = 0;
                }
            }

           
            sendmmsg(fd, output, num_udp_to_send, 0);

        }
    }

    return 0;
}

//--------------------------------------------------------------
//--------------------------------------------------------------
//--------------------------------------------------------------

void initialize_alarm(long timer_period){

    
	// How many nanoseconds do we wait between reads. Note:  1000 nanoseconds = 1us
    int num_microseconds = timer_period / 1000;

    printf("[%s] Setting the broadcast rate to %d microseconds...\n", NICKNAME, num_microseconds);


    static struct itimerval rtTimer;

    rtTimer.it_value.tv_sec = 0;
    rtTimer.it_value.tv_usec = num_microseconds;
    rtTimer.it_interval.tv_sec = 0;
    rtTimer.it_interval.tv_usec = num_microseconds;
    if (setitimer(ITIMER_REAL, &rtTimer, NULL) != 0) {
        printf("[%s] Error setting timer. \n", NICKNAME);
        exit(1);
    }

}

int initialize_broadcast_socket() {

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

    char broadcast_ip_string[INET_ADDRSTRLEN] = "192.168.137.255";
    uint32_t broadcast_ip = parse_ip_str(broadcast_ip_string);
    int broadcast_port = 51002;
    char interface[20] = "enp3s0f0";

    printf("[%s] Emitting on IP: %s, port: %d.\n", NICKNAME, broadcast_ip_string, broadcast_port);
    printf("[%s] Bind socket to device: %s.\n", NICKNAME, interface);

    int so = setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &interface,
        sizeof(interface));
    if (so < 0) {
        perror("[generator] interface binding error");
    }

    struct sockaddr_in addr;
    memset(&addr,0,sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = broadcast_ip;
    addr.sin_port        = htons(broadcast_port);

    // Connect here instead of using sentto because it's faster; kernel doesn't need to make
    // the necessary checks because it already has a valid file descriptor for the socket
    if (connect(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        perror("[cb_generator] connect error");
        exit(EXIT_FAILURE);
    }
    

    return fd;
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

//--------------------------------------------------------------
//--------------------------------------------------------------
//--------------------------------------------------------------
void initialize_signals() {

    printf("Attempting to initialize signal handlers.\n");

    signal(SIGINT, &handler_SIGINT);
    signal(SIGALRM, &handler_SIGALRM);

    printf("Signal handlers installed.\n");
}

void handler_SIGINT(int signum) {
    flag_SIGINT++;
}
void handler_SIGALRM(int signum) {
    flag_SIGALRM++;
}



int initialize_buffer(char **buffer, int num_channels, int ramp_max) {

    int buffer_size = (sizeof(cerebus_packet_header_t) + num_channels*2) * ramp_max;
    *buffer = malloc(buffer_size);

    printf("[%s] Memory allocated for data buffer.\n", NICKNAME);
    printf("[%s] Ramp goes from 0 to %d.\n", NICKNAME, ramp_max);

    uint16_t data = 0;
    uint32_t current_byte = 0;

    for (int i = 0; i < ramp_max; i++) {
     
        memset(&(*buffer)[current_byte], data, sizeof(data) * num_channels);
        current_byte += sizeof(data) * num_channels;
        data++;
    }

    printf("[%s] Buffer filled with ramp data.\n", NICKNAME);

    return buffer_size;

}
