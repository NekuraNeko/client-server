/** Lab5: Server 11/18/2017 for CS 591.
 * 
 * Author: Taylor Flatt
 * 
 * Properties:
 *   -- parallel/concurrent server.
 *   -- uses epoll in order to handle read/writes.
 *   -- uses two pthreads in order to handle clients.
 *           (1) conducts the handshake and verification and is temporary.
 *           (2) conducts the reading/writing between all clients and is permanent (epoll).
 *   -- uses read()/write() for all I/O.
 *   -- forces reads/writes to be in non-blocking mode.
 *   -- sets SIGCHLD to be ignored in server to avoid having to collect client handling subprocesses.
 *   -- handles broken/malicious clients in the handshake process (timeout).
 * 
 * Remarks:
 *   -- In order to compile, gcc requires -pthread for pthreads and -lrt for timers.
 * 
 * Usage: server
*/

/** Checklist
 *  (1) Ctrl + C results in a segfault and exits the server. Obviously something is going wrong with 
 *      the exit procedure and/or additional tasks are being run after memory has been ridded.
 *  (2) Partial writes will hang on the server causing all I/O to cease for all clients. One observation 
 *      is that if a single character is sent, the server picks it up and will perform the write.
 *  (3) Finish the timerfds.
 * 
 */

#define _POSIX_C_SOURCE 200809L // Required for timers.
#define _XOPEN_SOURCE 700 // Required for pty.
#define _GNU_SOURCE // Required for syscall.

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include "DTRACE.h"
#include "tpool.h"

/* Preprocessor constants. */
#define PORT 4070
#define MAX_LENGTH 4096
#define MAX_NUM_CLIENTS 64000
#define MAX_EVENTS 24
#define SECRET "cs407rembash\n"
#define CHALLENGE "<rembash>\n"
#define PROCEED "<ok>\n"
#define ERROR "<error>\n"

/* Client object. */
typedef enum cstate {
    new,
    validated,
    established,
    unwritten,
    terminated
} cstate_t;

typedef struct client {
    int                 socket_fd;
    int                 pty_fd;
    cstate_t            state;
    char                unwritten[MAX_LENGTH];
    size_t              nunwritten;  /* Size of the unwritten buffer. */
    int 				timer_fd;
} client_t;

/* Prototypes. */
int create_server();
void handle_io(int fd);
void client_connect();
cstate_t get_cstate(int fd);
int register_client(int sock);
int initiate_handshake(int client_fd);
int validate_client(int client_fd);
int open_pty(int client_fd);
int create_bash_process(char *pty_slave);
void epoll_listener();
int set_nonblocking_fd(int fd);
char *read_client_message(int client_fd);
void transfer_data(int from);
void graceful_exit(int exit_status);

/* Global Variables */
/* A map to store the clients. */
client_t *client_fd_tuples[MAX_NUM_CLIENTS * 2 + 5];
client_t client_fd_tuples_mem[MAX_NUM_CLIENTS * 2 + 5];
int timer_fd_tuples[MAX_NUM_CLIENTS * 2 + 5];

/* Epoll fds. */
int epoll_fd;
int t_epoll_fd;

/* Allows epoll to perform the client handshake if an event comes in on the listening socket. */
int listen_fd;

int main(int argc, char *argv[]) {

    tpool_init(handle_io);

    if((epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
        perror("(Main) epoll_create1(): Error creating EPOLL.");
        exit(EXIT_FAILURE);
    }
    
    if((t_epoll_fd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
        perror("(Main) epoll_create1(): Error creating T-EPOLL.");
        exit(EXIT_FAILURE);
    }

    if((create_server()) == -1) {
        perror("(Main) create_server(): Error creating the server.");
        exit(EXIT_FAILURE);
    }

    /* Forces writes to closed sockets to return an error rather than a signal. */
    if(signal(SIGPIPE,SIG_IGN) == SIG_ERR) {
        perror("(Main) signal(): Error setting SIGPIPE to SIG_IGN.");
        exit(EXIT_FAILURE);
    }

    /* Forces child processes to be automatically discarded when they terminate. */
    if(signal(SIGCHLD, SIG_IGN) == SIG_ERR) {
        perror("(Main) signal(): Error setting SIGCHLD to SIG_IGN.");
        exit(EXIT_FAILURE);
    }

    /* MAKE METHOD */
    /* Setup epoll for connection listener. */
    struct epoll_event ev;
    ev.events = EPOLLONESHOT | EPOLLIN | EPOLLET;
    ev.data.fd = t_epoll_fd;

    DTRACE("%ld:Setting epoll timerfd=%d.\n", (long)getpid(), t_epoll_fd);

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, t_epoll_fd, &ev) == -1) {
        perror("(create_server) epoll_ctl(): Failed to add socket to epoll.");
        exit(EXIT_FAILURE);
    }

    epoll_listener();
    

    exit(EXIT_FAILURE);
}

/** Creates the server by setting up the socket and begins listening.
 * 
 * Returns: An integer corresponding to server's file descriptor on success or failure -1.
*/
int create_server() {
    struct sockaddr_in server_address;

    if((listen_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("(create_server) socket(): Error creating socket.");
    }

    DTRACE("%ld:Starting server with fd=%d.\n", (long)getpid(), listen_fd);

    int i = 1;
    if(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i))) {
        perror("(create_server) setsockopt(): Error setting sockopt.");
        return -1;
    }

    /* Name the socket and set the port. */
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(PORT);

    if((bind(listen_fd, (struct sockaddr *) &server_address, sizeof(server_address))) == -1){
        perror("(create_server) bind(): Error assigning address to socket.");
        return -1;
    }

    if((listen(listen_fd, 10)) == -1){
        perror("(create_server) listen(): Error listening to socket.");
        return -1;
    }

    if(set_nonblocking_fd(listen_fd) == -1) {
        perror("(create_server) set_nonblocking_fd(): Error setting listen_fd to non-blocking.");
    }

    /* Setup epoll for connection listener. */
    struct epoll_event ev;
    ev.events = EPOLLONESHOT | EPOLLIN | EPOLLET;
    ev.data.fd = listen_fd;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        perror("(create_server) epoll_ctl(): Failed to add socket to epoll.");
        return -1;
    }

    return 0;
}

void handle_io(int fd) {

    //DTRACE("%ld:IO Discovered on fd=%d.\n", (long)getpid(), fd);

    if(fd == listen_fd) {
        client_connect();
    } else if(get_cstate(fd) == new) {
        if(validate_client(fd) || open_pty(fd)) {
            perror("(handle_io) validate_client()/establish_client(): Error establishing the client.");
            graceful_exit(fd);
        }
    } else if(get_cstate(fd) == terminated) {
			perror("(handle_io) DO NOTHING - CLIENT SHOULD BE TERMINATED");
	} else {
        transfer_data(fd);
    }
    
    /* Create a temporary epoll_event to rearm the fd. */
    struct epoll_event t_ev;
    
    if(fd != listen_fd) {

        /* If the client has been terminated, then do not process anything. */
        if(client_fd_tuples[fd] == NULL) {
            return;
        }

        client_t *client = client_fd_tuples[fd]; 
		
		if(client -> state == unwritten) {
			DTRACE("%ld:State of fd=%d is UNWRITTEN.\n", (long)getpid(), fd);
			t_ev.events = EPOLLOUT | EPOLLET | EPOLLONESHOT;
		} else {
			//DTRACE("%ld:State of fd=%d is ESTABLISHED.\n", (long)getpid(), fd);
			t_ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
		}
	} else {
		DTRACE("%ld:Rearming LISTENING fd=%d.\n", (long)getpid(), fd);
		t_ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
	}
    
    t_ev.data.fd = fd;

    //DTRACE("%ld:Rearming the fd=%d.\n", (long)getpid(), fd);

    if(epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &t_ev) == -1) {
        perror("(transfer_data) epoll_ctl(): Failed to modify socket in epoll to rearm with oneshot.");
        return;
    }
}

void client_connect() {

    int client_fd;

    if((client_fd = accept(listen_fd, (struct sockaddr *) NULL, NULL)) == -1 && errno != EAGAIN) {
        perror("(client_connect) accept(): Error making a connection with the client.");
        return;
    }

    if(client_fd == -1) {
        perror("STOP");
        return;
    }

    // What if we have too many clients on our server for our client_fd_tuples?

    if(register_client(client_fd)) {
        perror("(client_connect) register_client(): Failed to register the client with the server.");
        close(client_fd);
    }

    if(initiate_handshake(client_fd)) {
        perror("(client_connect) initiate_handshake(): Failed to initiate the handshake with the client.");
        close(client_fd);
    }
}

cstate_t get_cstate(int fd) {

    return client_fd_tuples[fd] -> state;
}


int register_client(int sock) {

    DTRACE("%ld:Begun registering CLIENT=%d.\n", (long)getpid(), sock);

    struct epoll_event ev;
    client_t *client = &client_fd_tuples_mem[sock];
    client -> socket_fd = sock;
    client -> state = new;
    client -> pty_fd = -1;      /* No pty created for the client yet. */
    client -> nunwritten = 0;   /* No unwritten data in its buffer yet. */

    client_fd_tuples[sock] = client;

    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = sock;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &ev) == -1) {
        perror("(register_client) epoll_ctl(): Failed to add socket to epoll.");
        return -1;
    }

    return 0;
}

int initiate_handshake(int client_fd) {
    
    DTRACE("%ld:Begun handshake with CLIENT=%d.\n", (long)getpid(), client_fd);

    /* Three second timer. */
    static struct itimerspec timer;
    timer.it_value.tv_sec = 3;
    timer.it_value.tv_nsec = 0;
    timer.it_interval.tv_sec = 0;
    timer.it_interval.tv_nsec = 0;
    int timer_fd;
    
    if(write(client_fd, CHALLENGE, strlen(CHALLENGE)) == -1) {
        perror("(initiate_handshake) write(): Server took too long sending message or write failed.");
        return -1;
    }

    /* Set non-blocking and close on exec. */
    if((timer_fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC)) == -1) {
        perror("(handshake) timer_create(): Error creating handshake timer.");
    }
    
    if(timerfd_settime(timer_fd, 0, &timer, NULL) == -1) {
		perror("(handshake) timerfd_settime(): Error setting handshake timer.");
    }
    
    DTRACE("%ld:Starting timer with fd=%d.\n", (long)getpid(), timer_fd);
    
    client_t *client = client_fd_tuples[client_fd];
    client -> timer_fd = timer_fd;

    /* Store the client fd in an array that is indexed by the timer fd so it can be found in the epoll loop. */
    timer_fd_tuples[timer_fd] = client_fd;
	
	struct epoll_event t_ev;
    t_ev.events = EPOLLONESHOT | EPOLLIN | EPOLLET;
    t_ev.data.fd = timer_fd;
    
    /* Add the timer to the timer epoll. */
	if(epoll_ctl(t_epoll_fd, EPOLL_CTL_ADD, timer_fd, &t_ev) == -1) {
        perror("(transfer_data) epoll_ctl(): Failed to modify socket in epoll to rearm with oneshot.");
        return -1;
    }

    return 0;
}

int validate_client(int client_fd) {

    DTRACE("%ld:Begun validation of CLIENT=%d.\n", (long)getpid(), client_fd);

    char *pass;

    if((pass = read_client_message(client_fd)) == NULL) {
        perror("(validate_client) read_client_message(): Reading the client's password failed.");
        return -1;
    }

    /* Set the client to a validated state so when the timer expires, they are not cleaned up. */
    client_t *client = client_fd_tuples[client_fd];
    client -> state = validated;

    if(strcmp(pass, SECRET) != 0) {
        perror("(validate_client) strcmp(): Server took too long comparing the challenge, the compare failed, or invalid secret.");
        write(client_fd, ERROR, strlen(ERROR));
        return -1;
    }

    return 0;
}

/** Opens the PTY and creates the connection between bash and the client by forking off a subprocess 
 * to run bash.
 * 
 * client_fd: An integer corresponding to the client's file descriptor.
 * 
 * Returns: An integer corresponding to the success 0, or failure -1.
*/
int open_pty(int client_fd) {
    
    char * pty_slave;
    int pty_master, err;
    struct epoll_event ev;
    client_t *client = client_fd_tuples[client_fd];

    DTRACE("%ld:Opening PTY for CLIENT=%d.\n", (long)getpid(), client_fd);  

    /** Open an unused pty dev and store the fd for later reference.
     * 
     * O_RDWR = Open pty for read/write.
     * O_NOCTTY = Don't make it a controlling terminal for the process.
     * O_CLOEXEC = Close the fd on exec.
    */
    if((pty_master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC)) == -1) {
        perror("(open_pty) posix_openpt(): Failed openpt.");
        return -1;        
    }

    /* Set pty_master to get closed when bash execs. */
    fcntl(pty_master, F_SETFD, FD_CLOEXEC);
    
    /** Attempt to kickstart the master.
     * 
     * Grantpt = Creates a child process that executes a set-user-ID-root
     *          program changing ownership of the slave to be the same as  
     *          the effective user ID of the calling process and changes 
     *          permissions so the owner has R/W permissions.
     * Unlockpt = Removes the internal lock placed on the slave corresponding 
     *          to the pty. (This must be after grantpt).
     * ptsname = Returns the name of the pty slave corresponding to the pty 
     *          master referred to by pty_master. (/dev/pts/nn).
     */
    if(grantpt(pty_master) == -1 || unlockpt(pty_master) == -1 || (pty_slave = ptsname(pty_master)) == NULL) {
        err = errno;        /* Preserve the error. */
        close(pty_master);
        errno = err;
        return -1;
    }

    pty_slave = (char *) malloc(1024);
    strcpy(pty_slave, ptsname(pty_master));

    /* Set the pty and client sockets to non-blocking mode. */
    if((set_nonblocking_fd(pty_master) || set_nonblocking_fd(client_fd)) == -1) {
        perror("(open_pty) set_nonblocking_fd(): Error setting client to non-blocking.");
    }

    /* Add the pty master to epoll with oneshot and edge-triggered enabled. */
    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    ev.data.fd = pty_master;

    if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, pty_master, &ev) == -1) {
        perror("(open_pty) epoll_ctl(): Failed to add PTY to epoll.");
        return -1;
    }

    /* Create bash subprocess. */
    if(fork() == 0) {
        DTRACE("%ld:PTY_MASTER=%i and PTY_SLAVE=%s.\n", (long)getppid(), pty_master, pty_slave);  
        
        close(pty_master);  /* No longer needed. Close it. */
        close(client_fd);   /* No longer needed. Close it. */

        if(create_bash_process(pty_slave) == -1) {
            perror("(open_pty) create_bash_process(): Failed to create bash process.");
        }

        /* It should never reach this point. */
        exit(EXIT_FAILURE);
    }

    /* Send the go-ahead message to the client. */
    write(client_fd, PROCEED, strlen(PROCEED));
    DTRACE("%ld:Completed handshake with CLIENT=%d.\n", (long)getpid(), client_fd);
    client -> state = established;

    if(client -> state == established) {
        DTRACE("%ld:Client state is now ESTABLISHED.\n", (long)getpid());
    }

    /* Set the pty fd for the client. */
    client -> pty_fd = pty_master;

    /* Add an entry into the map for the pty fd which is a copy of the client struct. */
    client_fd_tuples[pty_master] = client;

    DTRACE("%ld:SLAVE_PID=%d.\n", (long)getpid(), client->pty_fd);
    free(pty_slave);

    return 0;
}

/** Sets up a PTY for the bash process and redirects stdin, stdout, and stderr to the fd.
 * 
 * pty_slave: A string corresponding to a subprocess slave pty name.
 * 
 * Returns: An integer corresponding to the success 0, or failure -1.
*/
int create_bash_process(char *pty_slave) {

    int pty_slave_fd;

    if(setsid() == -1) {
        perror("(create_bash_process) setsid(): Could not create a new session.");
        return -1;            
    }

    /** Setup the pty for the bash subprocess.
     * 
     * O_RDWR = Open pty for read/write.
     * O_NOCTTY = Don't make it a controlling terminal for the process.
     * O_CLOEXEC = Close the fd on exec.
    */
    if((pty_slave_fd = open(pty_slave, O_RDWR | O_NOCTTY | O_CLOEXEC)) == -1) {
        perror("(create_bash_process) open(): Failed opening PTY_SLAVE.");
        return -1;
    }

    DTRACE("%ld:Creating bash and connecting it to SLAVE_FD=%i.\n", (long)getpid(), pty_slave_fd); 
    
    if ((dup2(pty_slave_fd, STDIN_FILENO) == -1) || (dup2(pty_slave_fd, STDOUT_FILENO) == -1) || (dup2(pty_slave_fd, STDERR_FILENO) == -1)) {
        perror("(create_bash_process) dup2(): Redirecting FD 0, 1, or 2 failed");
        exit(EXIT_FAILURE); 
    }

    close(pty_slave_fd);
    free(pty_slave);
    execlp("bash", "bash", NULL);

    DTRACE("%ld:Failed to exec bash on SLAVE_FD=%i.\n", (long)getpid(), pty_slave_fd); 

    return -1;
}

/** An epoll listener which handles communication between the client and server by 
 * waiting for read/write events to come through on available file descriptors.
 * 
 * ignore: A pointer which is a required argument. It is not used.
 * 
 * Returns: None.
*/
void epoll_listener() {

    struct epoll_event ev_list[MAX_EVENTS], t_ev_list[MAX_EVENTS];
    int events, t_events, timer_fd, client_fd;
    int i, j;

    while(1) {
        events = epoll_pwait(epoll_fd, ev_list, MAX_EVENTS, -1, 0);
        //events = epoll_wait(epoll_fd, ev_list, MAX_NUM_CLIENTS * 2, -1);

        //DTRACE("%ld:Sees EVENTS=%d from FD=%d.\n", (long)getpid(), events, ev_list[0].data.fd);

        for(i = 0; i < events; i++) {
            /* Check if there is an event and the associated fd is available for reading. */
            if(ev_list[i].events & (EPOLLIN | EPOLLOUT)) {
                /* If the event is a timer, process it. Otherwise transfer data. */
                if((ev_list[i].data.fd == t_epoll_fd) & EPOLLIN) { 
					t_events = epoll_pwait(t_epoll_fd, t_ev_list, MAX_EVENTS, -1, 0);
                    
                    /* Process the elapsed timers. */
					for(j = 0; j < t_events; j++) {
                        timer_fd = t_ev_list[j].data.fd;
                        client_fd = timer_fd_tuples[timer_fd];

                        /* If there is a timer event and it is a new client, then we need to release the client. */
                        if(client_fd_tuples[client_fd] && client_fd_tuples[client_fd] -> state == new) {
                            DTRACE("%ld:A timer has expired on t_epoll_fd=%d with timer_fd=%d for client=%d.\n", (long)getpid(), t_epoll_fd, t_ev_list[j].data.fd, client_fd);   

                            DTRACE("%ld:Closing timer fd=%d.\n", (long)getpid(), timer_fd);
                            if(epoll_ctl(t_epoll_fd, EPOLL_CTL_DEL, timer_fd, NULL) == -1) {
                                perror("(epoll_listener) epoll_ctl(): Failed to delete the timer fd in epoll.");
                            }

                            close(timer_fd);
                            graceful_exit(client_fd);
                        }
					}
                } else {
                    DTRACE("%ld:Adding task to the thread pool from fd=%d.\n", (long)getpid(), ev_list[i].data.fd); 
                    tpool_add_task(ev_list[i].data.fd);
                }
            } else if(ev_list[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                DTRACE("%ld:Received an EPOLLHUP or EPOLLERR on %d. Shutting it down.\n", (long)getpid(), ev_list[i].data.fd);
                graceful_exit(ev_list[i].data.fd);
            }
        }

        if(events == -1) {
            if(errno == EINTR) {
                continue;
            } else {
                perror("(epoll_listener) events_errno: Epoll loop error.");
                exit(EXIT_FAILURE);
            }
        }
    }
}

int set_nonblocking_fd(int fd) {
    
    int fd_flags;

    /* Get the current fd's flags. */
    if((fd_flags = fcntl(fd, F_GETFL, 0)) == -1) {
        perror("(set_nonblocking_fd) fcntl(): Error getting fd_flags.");
        return -1;
    }

    /* Append the non-blocking flag. */
    fd_flags |= O_NONBLOCK;

    /* Overwrite the fd's flags. */
    if(fcntl(fd, F_SETFL, fd_flags) == -1) {
        perror("(set_nonblocking_fd) fcntl(): Error setting fd_flags.");
        return -1;
    }

    return 0;
}

/** Reads the handshake messages.
 * 
 * client_fd: An integer corresponding to the clients's file descriptor.
 * 
 * Returns: A null terminated string if successful, otherwise NULL if an error is
 *          encountered.
*/
char *read_client_message(int client_fd)
{
  static char msg[MAX_LENGTH];
  int nread;
  
    if ((nread = read(client_fd, msg, MAX_LENGTH - 1)) <= 0) {
        if (errno)
            perror("(read_client_message) read(): Error reading from the client socket");
        else
            perror("read_client_message() read(): Client closed connection unexpectedly\n");
            
        return NULL; 
    }

  msg[nread] = '\0';

  return msg;
}

/** Actually reads and writes data to and from sockets.
 * 
 * Remarks: Need to check for both since either can be set...
 * EWOULDBLOCK/EAGAIN: If a read is going to block.
 * 
 * from: Integer representing the source file descriptor (read).
 * to: Integer representing the targer file descriptor (write).
 * 
 * Returns: An integer corresponding to the success 0, or failure -1.
*/
void transfer_data(int from) {
    
    char buf[MAX_LENGTH];
    ssize_t nread, nwrite;
    client_t *client = client_fd_tuples[from];
    int to;

    /* Determine where the data should go. */
    if(from == client -> pty_fd) {
        to = client -> socket_fd;
    } else {
        to = client -> pty_fd;
    }

    if(client -> state == terminated) {
        return;
    }

    // TODO: I could include a pointer that points to the START of the unwritten data which 
    // would solve the problem of multiple partial writes which didn't completely write the 
    // unwritten buffer. However, write works by starting from the beginning of a buffer and 
    // writing n-bytes of data. So it MIGHT require the use of an additional buffer or a different 
    // function to work regardless. So it may not be useful to complicate the client struct
    // further and instead just shift the unwritten bytes to the beginning of the buffer for 
    // processing the next time.

    // The client has unwritten data.
    if(get_cstate(client -> socket_fd) == unwritten) {
        DTRACE("%ld:There is unwritten data on fd=%d with nunwritten=%d.\n", (long)getpid(), from, (int)client -> nunwritten);
        //DTRACE("%ld:Before UNWRITTEN write on fd=%d with BUFFER=%s.\n", (long)getpid(), from, client -> unwritten);
        if((nwrite = write(to, client -> unwritten, client -> nunwritten)) == -1) {
            perror("(transfer_data) write(): Failed writing partial write data.");
        }
        //DTRACE("%ld:After UNWRITTEN write on fd=%d with BUFFER=%s.\n", (long)getpid(), from, client -> unwritten);
        
        DTRACE("%ld:Unwritten fd=%d, nwrite=%d.\n", (long)getpid(), client -> socket_fd, (int)nwrite);

        // There is still data left in the buffer to write.
        if(client -> nunwritten > nwrite) {
			DTRACE("%ld:There is STILL unwritten data on fd=%d with nwrite=%d and nunwritten=%d.\n", (long)getpid(), from, (int)nwrite, (int)client -> nunwritten);
            client -> nunwritten -= nwrite;

            // Require memmove since the memory locations might overlap. This needs EXTENSIVE testing.
            memmove(client -> unwritten, client -> unwritten + nwrite, client -> nunwritten);
            //DTRACE("%ld:After MEMMOVE write on fd=%d with BUFFER=%s.\n", (long)getpid(), from, client -> unwritten);

        } else {
			DTRACE("%ld:Unwritten data has been completely written for fd=%d.\n", (long)getpid(), from);
            client -> nunwritten = 0;
            client -> state = established;
        }
    } else {
        if((nread = read(from, buf, MAX_LENGTH)) > 0) {
			//DTRACE("%ld:Before NORMAL write on fd=%d with BUFFER=%s.\n", (long)getpid(), from, buf);
            nwrite = write(to, buf, nread);
            //DTRACE("%ld:After NORMAL write on fd=%d with BUFFER=%s.\n", (long)getpid(), from, buf);
        }

        if(nread == -1 && errno != EWOULDBLOCK && errno != EAGAIN) {
            DTRACE("%ld:Error read()'ing from FD %d\n", (long)getpid(), from);
            perror("(transfer_data) nread_errno: Failed reading data.");
            graceful_exit(from);
        }

        if(nread == 0) {
            DTRACE("%ld:NREAD=0 The socket was closed.\n", (long)getpid());
            graceful_exit(from);
        }
        
        if(nwrite == -1 && errno != EWOULDBLOCK && errno != EAGAIN) {
			DTRACE("%ld:Error write()'ing from FD %d\n", (long)getpid(), from);
            perror("(transfer_data) nwrite_errno: Failed writing data.");
            graceful_exit(from);
		}
		
		if(nwrite == -1) {
			nwrite = 0;
		}

        // There is a partial write. Store the unwritten data in the client buffer and change state.
        if(nwrite < nread) {
			DTRACE("\n\n\n\n\n\n\n");
			DTRACE("%ld:WARN! Unwritten on fd=%d with nwrite=%d and nread=%d.\n", (long)getpid(), from, (int)nwrite, (int)nread);
			DTRACE("\n\n\n\n\n\n\n");
			
            client -> nunwritten = nread - nwrite;
            memcpy(client -> unwritten, buf + nwrite, client -> nunwritten);
            // buffer: ABCDEF
            // read: 6
            // write: 2
            // nunwritten: 6 - 2 = 4
            // unwritten: CDEF
            // ERROR -> unwritten: ABCD
            client -> state = unwritten;
        }
    }

    return;
}

/** Called in order to close file descriptors when the client is finished.
 * 
 * fd: An integer representing the file descriptor that will be closed.
 * 
 * Returns: None.
*/
void graceful_exit(int fd) {

    DTRACE("%ld:Started exit procedure.\n", (long)getpid());

    if(client_fd_tuples[fd] == NULL) {
        DTRACE("%ld:ERROR!!!!.\n", (long)getpid());
        return;
    }

    client_t *client = client_fd_tuples[fd];
    int client_fd = client -> socket_fd;    

    /* If we haven't completed client setup, just close the fd. */
    if(client -> state == new) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
        epoll_ctl(t_epoll_fd, EPOLL_CTL_DEL, client -> timer_fd, NULL);

        if(close(client_fd) != -1) {
            client_fd_tuples[client_fd] = NULL;
        }

        return;
    }

    client -> state = terminated;    
    
    DTRACE("%ld:Closing fd=%ld.\n", (long)getpid(), (long)client_fd);
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);

    /* Close the client fd and remove the client's reference in the struct. */
    if(close(client_fd) != -1) {
        client_fd_tuples[client_fd] = NULL;
    }

    int pty_fd = client -> pty_fd;

    /* Close the pty fd and remove the pty's reference in the struct. */
    DTRACE("%ld:Closing fd=%ld.\n", (long)getpid(), (long)pty_fd);
    if(client -> state == terminated) {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, pty_fd, NULL);

        if(close(pty_fd) != -1) {
            client_fd_tuples[pty_fd] = NULL;
        }
    }
}
