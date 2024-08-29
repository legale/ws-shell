#include <errno.h>
#include <fcntl.h>
#include <libwebsockets.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VERSION "0.2.0"
#define BUFFER_SIZE 4096

struct per_session_data {
  struct lws *wsi;
  int pty_fd;
  pid_t child_pid;
  unsigned char buffer[LWS_PRE + BUFFER_SIZE];
  size_t buffer_len;
  int write_pending;
  const char *command;
};

const char *global_command;
volatile int force_exit = 0;
int keep_running_after_disconnect = 0;
int ws_log_level = LLL_ERR | LLL_WARN; // Default log level

static int callback_shell(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len) {
  struct per_session_data *pss = (struct per_session_data *)user;
  int n;

  switch (reason) {
  case LWS_CALLBACK_ESTABLISHED:
    printf("Connection established\n");
    pss->command = global_command;
    pss->child_pid = forkpty(&pss->pty_fd, NULL, NULL, NULL);
    if (pss->child_pid == -1) {
      perror("forkpty failed");
      return -1;
    }
    if (pss->child_pid == 0) {
      printf("Child process: Executing command\n");
      execl("/bin/sh", "/bin/sh", "-c", pss->command, NULL);
      perror("execl failed");
      exit(1);
    }
    if (fcntl(pss->pty_fd, F_SETFL, O_NONBLOCK) == -1) {
      perror("fcntl failed");
      exit(1);
    }
    printf("PTY created and set to non-blocking mode\n");
    pss->buffer_len = 0;
    pss->write_pending = 0;
    lws_callback_on_writable(wsi);
    break;

  case LWS_CALLBACK_RECEIVE:
    printf("recv %zu: '%.*s' 0x%02x\n", len, (int)len, (char *)in,
           *(unsigned char *)in);
    if (pss->pty_fd >= 0) {
      if (strncmp(in, "RESIZE:", 7) == 0) {
        int cols, rows;
        sscanf(in + 7, "%d:%d", &cols, &rows);
        struct winsize ws;
        ws.ws_col = cols;
        ws.ws_row = rows;
        if (ioctl(pss->pty_fd, TIOCSWINSZ, &ws) == -1) {
          perror("ioctl TIOCSWINSZ failed");
        } else {
          printf("Resized terminal to %d cols and %d rows\n", ws.ws_col,
                 ws.ws_row);
        }
      } else {
        n = write(pss->pty_fd, in, len);
        if (n < 0) {
          perror("write to pty failed");
        } else {
          printf("Wrote %d bytes to PTY\n", n);
        }
      }
    }
    lws_callback_on_writable(wsi);
    break;

  case LWS_CALLBACK_SERVER_WRITEABLE:
    if (pss->pty_fd >= 0) {
      n = read(pss->pty_fd, pss->buffer + LWS_PRE,
               sizeof(pss->buffer) - LWS_PRE);
      if (n < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
          perror("read from pty failed");
          return -1;
        }
      } else if (n > 0) {
        printf("sent %d: '", n);
        for (int i = 0; i < n; i++) {
          printf("%02x ", pss->buffer[LWS_PRE + i]);
        }
        printf("'\n");
        pss->buffer_len = n;
        n = lws_write(wsi, pss->buffer + LWS_PRE, pss->buffer_len,
                      LWS_WRITE_TEXT);
        if (n < 0) {
          perror("lws_write failed");
          return -1;
        }
        printf("Wrote %d bytes to WebSocket\n", n);
        pss->buffer_len = 0;
        pss->write_pending = 1;
      } else {
        pss->write_pending = 0;
      }
    }
    if (pss->write_pending) {
      lws_callback_on_writable(wsi);
    }
    break;

  case LWS_CALLBACK_CLOSED:
    printf("Connection closed\n");
    if (pss->child_pid > 0) {
      printf("Terminating child process %d\n", pss->child_pid);
      kill(pss->child_pid, SIGTERM);
      pss->child_pid = -1;
    }
    if (pss->pty_fd >= 0) {
      printf("Closing PTY file descriptor %d\n", pss->pty_fd);
      close(pss->pty_fd);
      pss->pty_fd = -1;
    }
    if (!keep_running_after_disconnect) {
      force_exit = 1;
    }
    break;

  default:
    break;
  }

  return 0;
}

static struct lws_protocols protocols[] = {
    {.name = "shell",
     .callback = callback_shell,
     .per_session_data_size = sizeof(struct per_session_data),
     .rx_buffer_size = BUFFER_SIZE,
     .id = 0,
     .user = NULL,
     .tx_packet_size = 0},
    {NULL, 0, 0, 0, 0, NULL, 0},
};

void usage(const char *prog_name) {
  fprintf(stderr,
          "Usage: %s <port> <command> [keep_running_after_disconnect] "
          "[log_level]\n",
          prog_name);
  fprintf(stderr, "  <port>    : Port number for WebSocket server\n");
  fprintf(stderr, "  <command> : Command to execute in PTY\n");
  fprintf(
      stderr,
      "  [keep_running_after_disconnect] : Optional, 0 or 1 (default: 0)\n");
  fprintf(
      stderr,
      "  [log_level] : Optional, log level (default: LLL_ERR | LLL_WARN: %d)\n",
      ws_log_level);
  fprintf(stderr, "                insane log: %d\n",
          LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG | LLL_PARSER |
              LLL_HEADER | LLL_EXT | LLL_CLIENT | LLL_LATENCY | LLL_USER);
  fprintf(
      stderr,
      "                LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO log: %d\n",
      LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO);
}

int main(int argc, char **argv) {
  struct lws_context_creation_info info;
  struct lws_context *context;
  int port;

  if (argc < 3) {
    usage(argv[0]);
    return 1;
  }

  port = atoi(argv[1]);
  global_command = argv[2];

  if (argc > 3) {
    keep_running_after_disconnect = atoi(argv[3]);
  }

  if (argc > 4) {
    ws_log_level = atoi(argv[4]);
  }

  lws_set_log_level(ws_log_level, NULL);

  memset(&info, 0, sizeof(info));
  info.port = port;
  info.protocols = protocols;
  info.gid = -1;
  info.uid = -1;
  info.options = LWS_SERVER_OPTION_VALIDATE_UTF8;

  printf("Creating libwebsockets context\n");
  context = lws_create_context(&info);
  if (!context) {
    fprintf(stderr, "lws init failed\n");
    return 1;
  }

  // check port
  int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    perror("socket creation failed");
    return 1;
  }

  // set option SO_REUSEADDR
  int opt = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
    perror("setsockopt(SO_REUSEADDR) failed");
    close(sock_fd);
    lws_context_destroy(context);
    return 1;
  }

  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port); // Используем порт из аргументов или конфигурации
  addr.sin_addr.s_addr = INADDR_ANY;

  // check is port is bound
  if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    if (errno == EADDRINUSE) {
      printf("WebSocket server successfully bound to port %d\n", port);
    } else if (errno == EACCES) {
      fprintf(stderr, "Permission denied to bind to port %d\n", port);
      lws_context_destroy(context);
      close(sock_fd);
      return 1;
    } else {
      perror("Port check failed (bind)");
      close(sock_fd);
      lws_context_destroy(context);
      return 1;
    }
  } else {
    fprintf(stderr, "Port %d error\n", port);
    close(sock_fd);
    lws_context_destroy(context);
    return 1;
  }

  printf("WebSocket server for '%s' started on port %d\n",

         global_command, port);

  while (!force_exit) {
    int n = lws_service(context, 50);
    if (n < 0) {
      fprintf(stderr, "lws_service failed: %d\n", n);
      break;
    }
  }

  printf("Destroying libwebsockets context\n");
  lws_context_destroy(context);

  return 0;
}
