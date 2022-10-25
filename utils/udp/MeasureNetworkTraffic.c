/** The goal of this program is to measure the rate at which UDP packets are received at a port. 

    A non-blocking UDP socket is created. A while loop reads from the file-descriptor continuously. The amount of data in the payload is measured.  A signal is thrown every second. The display (ncurses) is updated every second.

	The program is called as follows:

		MeasureNetworkTraffic IP port

	So, for example, if would be called as follows:

		./MeasureNetworkTraffic 192.168.137.1 51001
	
	Version 1: David Brandman, April 2018
    Version 2: David Brandman, September 2022
*/

#include <signal.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>
#include <ncurses.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

// This is the total number of bytes that have been read 
int bytesRead = 0;
int numPackets = 0;

char *ip;
int port;

static void sigHandler(int sig)
{
	clear();
	printw("Listening on: %s, port: %d\n", ip, port);	
	printw("MegaBytes   per second: %.03f\n", bytesRead / 1000000.0);
	printw("Num Packets per second: %d\n", numPackets);
	
//	int packetLength = bytesRead == 0 ? 0 : bytesRead / numPackets;
//		printw("Packet Length: %d\n", packetLength);

	refresh();
	
	bytesRead = 0;	
	numPackets = 0;
}

int main(int argc, char *argv[])
{
	//InitializeDisplay(argc, argv);
	//SetVerbose(FALSE);
	

    //----------------------------------------------------------------
    // READ THE COMMANDLINE ARGUMENTS AND BIND TO A SOCKET
    //----------------------------------------------------------------

	if (argc > 1){
		 ip = argv[1];
	} 
	else {
		printf("Please provide an IP address. Exiting!\n");
		exit(1);
	}
 	
	if (argc > 2){
		port = atoi(argv[2]);
	} else
	{
		printf("Please provide a port. Exiting!\n");
		exit(1);
	}

    struct sockaddr_in sock_struct;
    memset(&sock_struct, 0, sizeof(struct sockaddr_in));
    sock_struct.sin_family = AF_INET;
    sock_struct.sin_port = htons(port);
    sock_struct.sin_addr.s_addr = inet_addr(ip);

    //----------------------------------------------------------------
    // Initialize and bind the Socket
    //----------------------------------------------------------------

	// Create the socket
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        printf("COULD NOT CREATE SOCKET. EXITING.\n");
        exit(1);
    }

    // Make the file-descriptor for the socket non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        printf("COULD NOT MAKE NON-BLOCKING SOCKET. EXITING.\n");
        exit(1);
    }

    // Bind the socket
    if(bind(fd, (struct sockaddr*) &sock_struct, sizeof(sock_struct)) < 0) {
        printf("COULD NOT BIND SOCKET. EXITING.\n");
        exit(1);
    }
    //----------------------------------------------------------------
    // Initialize the signal handler
    //----------------------------------------------------------------

	if(signal(SIGALRM, sigHandler) == SIG_ERR) {
		printf("Could not initialize Signal: %s", strerror(errno));
        exit(1);
    }

	// To make this work properly, we want to ignore the ALARM if it happens during the
	// readv() function call that happens as part of Supersocket. Otherwise funky stuff
	// happens if there is a signal thrown during a system call read.

	sigset_t blockSet, prevMask;
	sigemptyset(&blockSet); // Initialize this to be empty
	sigaddset(&blockSet, SIGALRM); // Now set the bit-mask flag to be active for SIGALRM

    //----------------------------------------------------------------
    // Initialize the timer
    //----------------------------------------------------------------

	struct itimerval t;
	t.it_interval.tv_sec = 1;
	t.it_interval.tv_usec = 0;
	t.it_value.tv_sec = 1;
	t.it_value.tv_usec = 0;

	if(setitimer(ITIMER_REAL, &t, 0) == -1) {
		printf("Could not initialize timer: %s\n", strerror(errno));
		exit(1);
	}

    //----------------------------------------------------------------
    // Main event
    //----------------------------------------------------------------

	int bufferSize = 2000;
    char buffer[bufferSize];

	initscr();

	while(1){

		if(sigprocmask(SIG_BLOCK, &blockSet, &prevMask) == -1){
			printf("Could not set signal mask: %s", strerror(errno));
			exit(1);
		}

        ssize_t n = recv(fd, &buffer, bufferSize, 0);
        if (n > 0) {
            bytesRead += n;
            numPackets++;
        }
		if(sigprocmask(SIG_SETMASK, &prevMask, NULL) == -1){
			printf("Could not reset signal mask: %s", strerror(errno));
			exit(1);
		}
		usleep(10);
	}

	
	return 0;
}
//    int one = 1;
//    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof(one)) < 0) {
//        printf("setsockopt(SO_REUSEADDR) failed");
//        exit(1);
//    }
