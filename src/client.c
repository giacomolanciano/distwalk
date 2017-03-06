#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include <pthread.h>
#include <netdb.h>

#include "message.h"
#include "timespec.h"

#include "cw_debug.h"
#include "expon.h"

int use_expon = 0;
int wait_spinning = 0;
int server_port = 7891;
int bind_port = 0;

void safe_send(int sock, unsigned char *buf, size_t len) {
  while (len > 0) {
    int sent;
    check(sent = send(sock, buf, len, 0));
    buf += sent;
    len -= sent;
  }
}

size_t safe_recv(int sock, unsigned char *buf, size_t len) {
  size_t read_tot = 0;
  while (len > 0) {
    int read;
    check(read = recv(sock, buf, len, 0));
    if (read == 0)
      break;
    buf += read;
    len -= read;
    read_tot += read;
  }
  return read_tot;
}

#define MAX_PKTS 1000000

clockid_t clk_id = CLOCK_REALTIME;
int clientSocket;
long usecs_send[MAX_PKTS];
long usecs_elapsed[MAX_PKTS];
// abs start-time of the experiment
struct timespec ts_start;
unsigned long num_pkts = 10;
unsigned long period_us = 1000;

char *hostname = "127.0.0.1";
char *bindname = "127.0.0.1";

void *thread_sender(void *data) {
  unsigned char send_buf[256];
  struct timespec ts_now;
  struct drand48_data rnd_buf;

  clock_gettime(clk_id, &ts_now);
  srand48_r(time(NULL), &rnd_buf);

  // Remember in ts_start the abs start time of the experiment
  ts_start = ts_now;

  for (int i = 0; i < num_pkts; i++) {
    /* remember time of send relative to ts_start */
    struct timespec ts_send;
    clock_gettime(clk_id, &ts_send);
    usecs_send[i] = ts_sub_us(ts_send, ts_start);
    /*---- Issue a request to the server ---*/
    message_t *m = (message_t *) send_buf;
    m->req_id = i;
    m->req_size = sizeof(send_buf);
    m->num = 2;
    m->cmds[0].cmd = COMPUTE;
    m->cmds[0].u.comp_time_us = 1;
    // Expect only req_id in message header as reply
    // TODO: support send and receive of variable reply-size requests
    m->cmds[1].cmd = REPLY;
    m->cmds[1].u.fwd.pkt_size = sizeof(message_t);
    cw_log("Sending %u bytes...\n", m->req_size);
    safe_send(clientSocket, send_buf, m->req_size);

    unsigned long period_ns;
    if (use_expon) {
      period_ns = lround(expon(1.0 / period_us, &rnd_buf) * 1000.0);
    } else {
      period_ns = period_us * 1000;
    }
    struct timespec ts_delta = (struct timespec) { period_ns / 1000000000, period_ns % 1000000000 };

    ts_now = ts_add(ts_now, ts_delta);

    if (wait_spinning) {
      struct timespec ts;
      do {
	clock_gettime(clk_id, &ts);
      } while (ts_leq(ts, ts_now));
    } else {
      check(clock_nanosleep(clk_id, TIMER_ABSTIME, &ts_now, NULL));
    }
  }

  return 0;
}

void *thread_receiver(void *data) {
  unsigned char recv_buf[256];
  for (int i = 0; i < num_pkts; i++) {
    /*---- Read the message from the server into the buffer ----*/
    // TODO: support receive of variable reply-size requests
    safe_recv(clientSocket, recv_buf, sizeof(message_t));
    message_t *m = (message_t *) recv_buf;
    unsigned long pkt_id = m->req_id;
    struct timespec ts_now;
    clock_gettime(clk_id, &ts_now);
    unsigned long usecs = (ts_now.tv_sec - ts_start.tv_sec) * 1000000
      + (ts_now.tv_nsec - ts_start.tv_nsec) / 1000;
    usecs_elapsed[pkt_id] = usecs - usecs_send[pkt_id];
    cw_log("Data received: %02lx (elapsed %ld us)\n", pkt_id, usecs_elapsed[pkt_id]);
  }

  for (int i = 0; i < num_pkts; i++) {
    printf("elapsed: %ld us\n", usecs_elapsed[i]);
  }

  return 0;
}

int main(int argc, char *argv[]) {
  int rv;
  struct sockaddr_in serveraddr;
  socklen_t addr_size;

  argc--;  argv++;
  while (argc > 0) {
    if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
      printf("Usage: client [-h|--help] [-b bindname] [-bp bindport] [-s servername] [-sb serverport] [-c num_pkts] [-p period(us)] [-e|--expon]\n");
      exit(0);
    } else if (strcmp(argv[0], "-s") == 0) {
      assert(argc >= 2);
      hostname = argv[1];
      argc--;  argv++;
    } else if (strcmp(argv[0], "-sp") == 0) {
      assert(argc >= 2);
      server_port = atoi(argv[1]);
      argc--;  argv++;
    } else if (strcmp(argv[0], "-b") == 0) {
      assert(argc >= 2);
      bindname = argv[1];
      argc--;  argv++;
    } else if (strcmp(argv[0], "-bp") == 0) {
      assert(argc >= 2);
      bind_port = atoi(argv[1]);
      argc--;  argv++;
    } else if (strcmp(argv[0], "-c") == 0) {
      assert(argc >= 2);
      num_pkts = atoi(argv[1]);
      assert(num_pkts <= MAX_PKTS);
      argc--;  argv++;
    } else if (strcmp(argv[0], "-p") == 0) {
      assert(argc >= 2);
      period_us = atol(argv[1]);
      argc--;  argv++;
    } else if (strcmp(argv[0], "-e") == 0 || strcmp(argv[0], "--expon") == 0) {
      use_expon = 1;
    } else if (strcmp(argv[0], "-ws") == 0 || strcmp(argv[0], "--waitspin") == 0) {
      wait_spinning = 1;
   } else {
      printf("Unrecognized option: %s\n", argv[0]);
      exit(-1);
    }
    argc--;  argv++;
  }

  printf("Configuration: bindname=%s:%d hostname=%s:%d num_pkts=%lu period_us=%lu expon=%d waitspin=%d\n", bindname, bind_port, hostname, server_port, num_pkts, period_us, use_expon, wait_spinning);

  cw_log("Resolving %s...\n", hostname);
  struct hostent *e = gethostbyname(hostname);
  check(e != NULL);
  cw_log("Host %s resolved to %d bytes: %s\n", hostname, e->h_length, inet_ntoa(*(struct in_addr *)e->h_addr));

  /* build the server's Internet address */
  bzero((char *) &serveraddr, sizeof(serveraddr));
  serveraddr.sin_family = AF_INET;
  bcopy((char *)e->h_addr, 
	(char *)&serveraddr.sin_addr.s_addr, e->h_length);
  serveraddr.sin_port = htons(server_port);

  /*---- Create the socket. The three arguments are: ----*/
  /* 1) Internet domain 2) Stream socket 3) Default protocol (TCP in this case) */
  clientSocket = socket(PF_INET, SOCK_STREAM, 0);

  int val = 0;
  setsockopt(clientSocket, IPPROTO_TCP, TCP_NODELAY, (void *)&val, sizeof(val));

  cw_log("Resolving %s...\n", bindname);
  struct hostent *e2 = gethostbyname(bindname);
  check(e2 != NULL);
  cw_log("Host %s resolved to %d bytes: %s\n", bindname, e2->h_length, inet_ntoa(*(struct in_addr *)e2->h_addr));

  struct sockaddr_in myaddr;
  myaddr.sin_family = AF_INET;
  /* Set IP address to resolved bindname */
  bcopy((char *)e2->h_addr,
	(char *)&myaddr.sin_addr.s_addr, e2->h_length);
  /* Set port to zero, requesting allocation of any available ephemeral port */
  myaddr.sin_port = bind_port;
  /* Set all bits of the padding field to 0 */
  memset(myaddr.sin_zero, '\0', sizeof(myaddr.sin_zero));

  cw_log("Binding to %s:%d\n", inet_ntoa(myaddr.sin_addr), myaddr.sin_port);

  /*---- Bind the address struct to the socket ----*/
  check(bind(clientSocket, (struct sockaddr *) &myaddr, sizeof(myaddr)));

  /*---- Connect the socket to the server using the address struct ----*/
  addr_size = sizeof(serveraddr);
  check(connect(clientSocket, (struct sockaddr *) &serveraddr, addr_size));

  pthread_t sender;
  pthread_create(&sender, NULL, thread_sender, (void *) 0);

  pthread_t receiver;
  pthread_create(&receiver, NULL, thread_receiver, NULL);

  pthread_join(sender, (void **) &rv);
  pthread_join(receiver, (void **) &rv);

  return 0;
}
