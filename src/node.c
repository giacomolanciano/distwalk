#include "message.h"
#include "timespec.h"
#include "cw_debug.h"

#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <errno.h>

#include <pthread.h>
#include <sys/epoll.h>

typedef enum { RECEIVING, SENDING, FORWARDING, LOADING, STORING, CONNECTING } req_status;

typedef struct {
  unsigned char *buf;		// NULL for unused buf_info
  unsigned long buf_size;
  unsigned char *curr_buf;
  unsigned long curr_size;
  int sock;
  req_status status;
  int orig_sock_id;             // ID in socks[]
  int parent_buf_id;            // used in FORWARDING, identifies the entry in bufs[] related to the client that issued the forward
  int parent_cmd_id;            // same
} buf_info;

#define MAX_BUFFERS 16

buf_info bufs[MAX_BUFFERS];

typedef struct {
  in_addr_t inaddr;	// target IP
  uint16_t port;	// target port (for multiple nodes on same host)
  int sock;		// socket handling messages from/to inaddr:port (0=unused)
} sock_info;

#define MAX_SOCKETS 16
sock_info socks[MAX_SOCKETS];

char *bind_name = "0.0.0.0";
int bind_port = 7891;

int no_delay = 1;

int epollfd;

void setnonblocking(int fd) {
   int flags = fcntl(fd, F_GETFL, 0);
   assert(flags >= 0);
   flags |= O_NONBLOCK;
   assert(fcntl(fd, F_SETFL, flags) == 0);
}

// return sock associated to inaddr:port
int sock_find_addr(in_addr_t inaddr, int port) {
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (socks[i].inaddr == inaddr && socks[i].port == port) {
      return socks[i].sock;
    }
  }
  return -1;
}

// return index of sock in socks[]
int sock_find_sock(int sock) {
  assert(sock != -1);
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (socks[i].sock == sock) {
      return i;
    }
  }
  return -1;
}

// add the IP/port into the socks[] map to allow FORWARD finding an
// already set-up socket, through sock_find()
// FIXME: bad complexity with many sockets
int sock_add(in_addr_t inaddr, int port, int sock) {
  int sock_id = sock_find_addr(inaddr, port);
  if (sock_id != -1)
    return sock_id;
  for (int i = 0; i < MAX_SOCKETS; i++) {
    if (socks[i].sock == -1) {
      socks[i].inaddr = inaddr;
      socks[i].port = port;
      socks[i].sock = sock;
      return i;
    }
  }
  return -1;
}

void sock_del_id(int id) {
  assert(id < MAX_SOCKETS);
  cw_log("marking socks[%d] invalid\n", id);
  socks[id].sock = -1;
}

// make entry in socks[] associated to sock invalid, return entry ID if found or -1
int sock_del(int sock) {
  int id = sock_find_sock(sock);
  if (id == -1)
    return -1;
  sock_del_id(id);
  return id;
}

// return ID of free buf_info in bufs[], or -1
int new_buf_info() {
  for (int buf_id = 0; buf_id < MAX_BUFFERS; buf_id++)
    if (bufs[buf_id].buf == 0) {
      bufs[buf_id].buf = malloc(BUF_SIZE);
      if (bufs[buf_id].buf == 0) {
        fprintf(stderr, "Not enough memory for allocating new buffer!\n");
        return -1;
      }
      bufs[buf_id].buf_size = BUF_SIZE;
      return buf_id;
    }
  return -1;
}

void safe_send(int sock, unsigned char *buf, size_t len) {
  while (len > 0) {
    int sent;
    sys_check(sent = send(sock, buf, len, 0));
    buf += sent;
    len -= sent;
  }
}

size_t safe_recv(int sock, unsigned char *buf, size_t len) {
  size_t read_tot = 0;
  while (len > 0) {
    int read;
    sys_check(read = recv(sock, buf, len, 0));
    if (read == 0)
      return read_tot;
    buf += read;
    len -= read;
    read_tot += read;
  }
  return read_tot;
}

unsigned char fwd_buf[BUF_SIZE];

// Copy m header into m_dst, skipping the first cmd_id elems in m_dst->cmds[]
void copy_tail(message_t *m, message_t *m_dst, int cmd_id) {
  // copy message header
  *m_dst = *m;
  // left-shift m->cmds[] into m_dst->cmds[], removing m->cmds[cmd_id]
  for (int i = cmd_id; i < m->num; i++) {
    m_dst->cmds[i - cmd_id] = m->cmds[i];
  }
  m_dst->num = m->num - cmd_id;
}

int remember(in_addr_t inaddr, int port, int sock) {
  if (sock_add(inaddr, port, sock) == -1)
    return -1;
  struct epoll_event ev;
  ev.events = EPOLLIN | EPOLLOUT;
  ev.data.fd = sock;
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sock, &ev) == -1) {
    perror("epoll_ctl: listen_sock");
    exit(EXIT_FAILURE);
  }
  return 0;
}

void forward_sock(int buf_id, int cmd_id, int sock) {
  unsigned char *buf = bufs[buf_id].buf;
  message_t *m = (message_t *) buf;
  message_t *m_dst = (message_t *) fwd_buf;
  copy_tail(m, m_dst, cmd_id + 1);
  m_dst->req_size = m->cmds[cmd_id].u.fwd.pkt_size;
  cw_log("Forwarding req %u to %s:%d\n", m->req_id,
	 inet_ntoa((struct in_addr) { m->cmds[cmd_id].u.fwd.fwd_host }),
	 m->cmds[cmd_id].u.fwd.fwd_port);
  cw_log("  cmds[] has %d items, pkt_size is %u\n", m_dst->num, m_dst->req_size);
  // TODO: return to epoll loop to handle sending of long packets
  // (here I'm blocking the thread)
  safe_send(sock, fwd_buf, m_dst->req_size);
}

// cmd_id is the index of the FORWARD item within m->cmds[] here, we
// remove the first (cmd_id+1) commands from cmds[], and forward the
// rest to the next hop
void forward(int buf_id, int cmd_id) {
  unsigned char *buf = bufs[buf_id].buf;
  message_t *m = (message_t *) buf;
  int sock = sock_find_addr(m->cmds[cmd_id].u.fwd.fwd_host, m->cmds[cmd_id].u.fwd.fwd_port);
  if (sock == -1) {
    sock = socket(PF_INET, SOCK_STREAM, 0);
    sys_check(setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void *)&no_delay, sizeof(no_delay)));
    setnonblocking(sock);
    struct sockaddr_in serveraddr;
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)&m->cmds[cmd_id].u.fwd.fwd_host, 
          (char *)&serveraddr.sin_addr.s_addr, sizeof(m->cmds[cmd_id].u.fwd.fwd_host));
    serveraddr.sin_port = htons(m->cmds[cmd_id].u.fwd.fwd_port);
    size_t addr_size = sizeof(serveraddr);
    int rv = connect(sock, (struct sockaddr *) &serveraddr, addr_size);
    if (rv == 0) {
      // unlikely in non-blocking mode
      remember(m->cmds[cmd_id].u.fwd.fwd_host, m->cmds[cmd_id].u.fwd.fwd_port, sock);
      forward_sock(buf_id, cmd_id, sock);
    } else {
      if (errno == -EINPROGRESS) {
        remember(m->cmds[cmd_id].u.fwd.fwd_host, m->cmds[cmd_id].u.fwd.fwd_port, sock);
        bufs[buf_id].status = CONNECTING;
      } else {
        perror("connect() failed: ");
        exit(EXIT_FAILURE);
      }
    }
  } else {
    bufs[buf_id].status = FORWARDING;
    forward_sock(buf_id, cmd_id, sock);
  }
}

unsigned char reply_buf[BUF_SIZE];

void reply(int sock, message_t *m, int cmd_id) {
  message_t *m_dst = (message_t *) reply_buf;
  copy_tail(m, m_dst, cmd_id + 1);
  m_dst->req_size = m->cmds[cmd_id].u.fwd.pkt_size;
  cw_log("Replying to req %u\n", m->req_id);
  cw_log("  cmds[] has %d items, pkt_size is %u\n", m_dst->num, m_dst->req_size);
  // TODO: return to epoll loop to handle sending of long packets
  // (here I'm blocking the thread)
  safe_send(sock, reply_buf, m_dst->req_size);
}

size_t recv_message(int sock, unsigned char *buf, size_t len) {
  assert(len >= 8);
  size_t read = safe_recv(sock, buf, 8);
  if (read == 0)
    return read;
  message_t *m = (message_t *) buf;
  assert(len >= m->req_size - 8);
  assert(safe_recv(sock, buf + 8, m->req_size - 8) == m->req_size - 8);
  return m->req_size;
}

void compute_for(unsigned long usecs) {
  struct timespec ts_beg, ts_end;
  cw_log("COMPUTE: computing for %lu usecs\n", usecs);
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_beg);
  do {
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts_end);
  } while (ts_sub_us(ts_end, ts_beg) < usecs);
}

int close_and_forget(int sock) {
  cw_log("removing sock=%d from epollfd\n", sock);
  if (epoll_ctl(epollfd, EPOLL_CTL_DEL, sock, NULL) == -1) {
    perror("epoll_ctl: deleting socket");
    exit(EXIT_FAILURE);
  }
  cw_log("removing sock=%d from socks[]\n", sock);
  sock_del(sock);
  return close(sock);
}

void process_messages(int sock, int buf_id) {
  size_t received = recv(sock, bufs[buf_id].curr_buf, bufs[buf_id].curr_size, 0);
  cw_log("recv() returned: %d\n", (int)received);
  if (received == 0) {
    cw_log("Connection closed by remote end\n");
    close_and_forget(sock);
    free(bufs[buf_id].buf);
    bufs[buf_id].buf = NULL;
    return;
  } else if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    cw_log("Got EAGAIN or EWOULDBLOCK, ignoring...\n");
    return;
  } else if (received == -1) {
    perror("Unexpected error!");
    exit(-1);
  }
  bufs[buf_id].curr_buf += received;
  bufs[buf_id].curr_size -= received;

  unsigned char *buf = bufs[buf_id].buf;
  unsigned long msg_size = bufs[buf_id].curr_buf - buf;
  // batch processing of multiple messages, if received more than 1
  do {
    cw_log("msg_size=%lu\n", msg_size);
    if (msg_size < sizeof(message_t)) {
      cw_log("Got incomplete header, need to recv() more...\n");
      break;
    }
    message_t *m = (message_t *) buf;
    cw_log("Received %lu bytes, req_id=%u, req_size=%u, num=%d\n", msg_size, m->req_id, m->req_size, m->num);
    if (msg_size < m->req_size) {
      cw_log("Got header but incomplete message, need to recv() more...\n");
      break;
    }
    assert(m->req_size >= sizeof(message_t) && m->req_size <= BUF_SIZE);
    for (int i = 0; i < m->num; i++) {
      if (m->cmds[i].cmd == COMPUTE) {
	compute_for(m->cmds[i].u.comp_time_us);
      } else if (m->cmds[i].cmd == FORWARD) {
	forward(buf_id, i);
	// rest of cmds[] are for next hop, not me
	break;
      } else if (m->cmds[i].cmd == REPLY) {
	reply(sock, m, i);
	// any further cmds[] for replied-to hop, not me
	break;
      } else {
	cw_log("Unknown cmd: %d\n", m->cmds[0].cmd);
	exit(-1);
      }
    }

    // move to batch processing of next message if any
    buf += m->req_size;
    msg_size = bufs[buf_id].curr_buf - buf;
    if (msg_size > 0)
      cw_log("Repeating loop with msg_size=%lu\n", msg_size);
  } while (msg_size > 0);

  if (buf == bufs[buf_id].curr_buf) {
    // all received data was processed, reset curr_* for next receive
    bufs[buf_id].curr_buf = bufs[buf_id].buf;
    bufs[buf_id].curr_size = bufs[buf_id].buf_size;
  } else {
    // leftover received data, move it to beginning of buf unless already there
    if (buf != bufs[buf_id].buf) {
      // TODO do this only if we're beyond a threshold in buf[]
      unsigned long leftover = bufs[buf_id].curr_buf - buf;
      cw_log("Moving %lu leftover bytes back to beginning of buf with buf_id %d",
	     leftover, buf_id);
      memmove(bufs[buf_id].buf, buf, leftover);
      bufs[buf_id].curr_buf = bufs[buf_id].buf + leftover;
      bufs[buf_id].curr_size = bufs[buf_id].buf_size - leftover;
    }
  }
}

void send_messages(int buf_id) {
  printf("send_messages()\n");
}

void finalize_conn(int buf_id) {
  printf("finalize_conn()\n");
  int val;
  socklen_t val_size = sizeof(val);
  sys_check(getsockopt(bufs[buf_id].sock, SOL_SOCKET, SO_ERROR, &val, &val_size));
  if (val != 0) {
    fprintf(stderr, "connect() failed: %s\n", strerror(val));
    exit(EXIT_FAILURE);
  }
  forward_sock(buf_id, cmd_id, sock);
}

void *receive_thread(void *data) {
  int sock = (int)(long) data;
  unsigned char buf[1024];

  while (1) {
    size_t msg_size = recv_message(sock, buf, sizeof(buf));
    if (msg_size == 0) {
      cw_log("Connection closed by remote end\n");
      break;
    }
    message_t *m = (message_t *) buf;
    cw_log("Received %lu bytes, req_id=%u, req_size=%u, num=%d\n", msg_size, m->req_id, m->req_size, m->num);
    if (m->num >= 1) {
      if (m->cmds[0].cmd == COMPUTE) {
	compute_for(m->cmds[0].u.comp_time_us);
      }
    }
    safe_send(sock, buf, sizeof(message_t));
  }
  sys_check(close_and_forget(sock));
  return 0;
}

#define MAX_EVENTS 10
void epoll_main_loop(int listen_sock) {
  struct epoll_event ev, events[MAX_EVENTS];

  /* Code to set up listening socket, 'listen_sock',
     (socket(), bind(), listen()) omitted */

  epollfd = epoll_create1(0);
  if (epollfd == -1) {
    perror("epoll_create1");
    exit(EXIT_FAILURE);
  }

  ev.events = EPOLLIN;
  ev.data.fd = -1;	// Special value denoting listen_sock
  if (epoll_ctl(epollfd, EPOLL_CTL_ADD, listen_sock, &ev) == -1) {
    perror("epoll_ctl: listen_sock");
    exit(EXIT_FAILURE);
  }

  for (;;) {
    cw_log("epoll_wait()ing...\n");
    int nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1);
    if (nfds == -1) {
      perror("epoll_wait");
      exit(EXIT_FAILURE);
    }

    for (int i = 0; i < nfds; i++) {
      if (events[i].data.fd == -1) {
	struct sockaddr_in addr;
	socklen_t addr_size = sizeof(addr);
	int conn_sock = accept(listen_sock,
			   (struct sockaddr *) &addr, &addr_size);
	if (conn_sock == -1) {
	  perror("accept");
	  exit(EXIT_FAILURE);
	}
	cw_log("Accepted connection from: %s:%d\n", inet_ntoa(addr.sin_addr), addr.sin_port);
	//setnonblocking(conn_sock);
	int val = 1;
	sys_check(setsockopt(conn_sock, IPPROTO_TCP, TCP_NODELAY, (void *)&val, sizeof(val)));

        int orig_sock_id = sock_add(addr.sin_addr.s_addr, addr.sin_port, conn_sock);
        check(orig_sock_id != -1);

	int buf_id = new_buf_info();
	if (buf_id == -1) {
	  fprintf(stderr, "Not enough buffers for new connection, closing!\n");
	  close_and_forget(conn_sock);
	  continue;
	}
	bufs[buf_id].curr_buf = bufs[buf_id].buf;
	bufs[buf_id].curr_size = BUF_SIZE;
	bufs[buf_id].sock = conn_sock;
        bufs[buf_id].status = RECEIVING;
        bufs[buf_id].orig_sock_id = orig_sock_id;

	ev.events = EPOLLIN | EPOLLOUT;
	// Use the data.u32 field to store the buf_id in bufs[]
	ev.data.u32 = buf_id;
	if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conn_sock,
		      &ev) == -1) {
	  perror("epoll_ctl: conn_sock");
	  exit(EXIT_FAILURE);
	}
      } else {
	// FIXME - allow for receiving only message parts, handle EAGAIN
	int buf_id = events[i].data.u32;
	cw_log("Receiving and processing on buf_id=%d...\n", buf_id);
        if ((events[i].events | EPOLLIN) && bufs[buf_id].status == RECEIVING)
          process_messages(bufs[buf_id].sock, buf_id);
        else if ((events[i].events | EPOLLOUT) && bufs[buf_id].status == SENDING)
          send_messages(buf_id);
        else if ((events[i].events | EPOLLOUT) && bufs[buf_id].status == CONNECTING)
          finalize_conn(buf_id);
        else {
          fprintf(stderr, "unexpected status: event=%d, %d\n", events[i].events, bufs[buf_id].status);
          exit(EXIT_FAILURE);
        }
      }
    }
  }
}

int main(int argc, char *argv[]) {
  int welcomeSocket;
  struct sockaddr_in serverAddr;

  argc--;  argv++;
  while (argc > 0) {
    if (strcmp(argv[0], "-h") == 0 || strcmp(argv[0], "--help") == 0) {
      printf("Usage: node [-h|--help] [-b bindname] [-bp bindport]\n");
      exit(0);
    } else if (strcmp(argv[0], "-b") == 0) {
      assert(argc >= 2);
      bind_name = argv[1];
      argc--;  argv++;
    } else if (strcmp(argv[0], "-bp") == 0) {
      assert(argc >= 2);
      bind_port = atol(argv[1]);
      argc--;  argv++;
    } else if (strcmp(argv[0], "-nd") == 0 || strcmp(argv[0], "--no-delay") == 0) {
      assert(argc >= 2);
      no_delay = atoi(argv[1]);
      argc--;  argv++;
    } else {
      printf("Unrecognized option: %s\n", argv[0]);
      exit(-1);
    }
    argc--;  argv++;
  }

  // Tag all buf_info as unused
  for (int i = 0; i < MAX_BUFFERS; i++) {
    bufs[i].buf = 0;
  }

  // Tag all sock_info as unused
  for (int i = 0; i < MAX_SOCKETS; i++) {
    socks[i].sock = -1;
  }

  /*---- Create the socket. The three arguments are: ----*/
  /* 1) Internet domain 2) Stream socket 3) Default protocol (TCP in this case) */
  welcomeSocket = socket(PF_INET, SOCK_STREAM, 0);

  int val = 1;
  setsockopt(welcomeSocket, IPPROTO_TCP, SO_REUSEADDR, (void *)&val, sizeof(val));
  setsockopt(welcomeSocket, IPPROTO_TCP, SO_REUSEPORT, (void *)&val, sizeof(val));

  /*---- Configure settings of the server address struct ----*/
  /* Address family = Internet */
  serverAddr.sin_family = AF_INET;
  /* Set port number, using htons function to use proper byte order */
  serverAddr.sin_port = htons(bind_port);
  /* Set IP address to localhost */
  serverAddr.sin_addr.s_addr = inet_addr(bind_name);
  /* Set all bits of the padding field to 0 */
  memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);  

  /*---- Bind the address struct to the socket ----*/
  sys_check(bind(welcomeSocket, (struct sockaddr *) &serverAddr, sizeof(serverAddr)));

  /*---- Listen on the socket, with 5 max connection requests queued ----*/
  sys_check(listen(welcomeSocket, 5));
  cw_log("Accepting new connections...\n");

  epoll_main_loop(welcomeSocket);

  return 0;
}
