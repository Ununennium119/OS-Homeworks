#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <unistd.h>

#include "libhttp.h"
#include "wq.h"

#define MAX_BODY_SIZE 4194304   // 4MB
#define READ_SIZE     32768     // 32KB


typedef struct proxy_details {
  int src_fd;
  int dest_fd;
  int *done;
  pthread_cond_t *cond_var;
} proxy_details_t;


/*
 * Global configuration variables.
 * You need to use these in your implementation of handle_files_request and
 * handle_proxy_request. Their values are set up in main() using the
 * command line arguments (already implemented for you).
 */
wq_t work_queue;
int num_threads;
int server_port;
char *server_files_directory;
char *server_proxy_hostname;
int server_proxy_port;


/*
 * Returns the path prepended by server_files_directory.
 */
char *get_complete_path(char *path) {
  size_t sfd_size = strlen(server_files_directory);
  size_t path_size = strlen(path);
  char *complete_path = (char *) malloc(2 + sfd_size + path_size + 1);
  if (!complete_path) {
    fprintf(stderr, "Malloc Failed\n");
    exit(ENOBUFS);
  }
  complete_path[0] = '.';
  complete_path[1] = '/';
  memcpy(complete_path + 2, server_files_directory, sfd_size);
  memcpy(complete_path + 2 + sfd_size, path, path_size + 1);

  return complete_path;
}

/*
 * Serves the contents of the file stored at `path` to the client socket `fd`.
 * It is the caller's reponsibility to ensure that the file stored at `path` exists.
 */
void serve_file(int fd, char *complete_path) {
  size_t file_size;
  FILE *file = fopen(complete_path, "rb");
  if (file) {
    /* get file size */
    struct stat path_stat;
    stat(complete_path, &path_stat);
    file_size = path_stat.st_size;

    /* convert file_size to string */
    char file_size_string[32];
    snprintf(file_size_string, 32, "%ld", file_size);

    /* send response */
    http_start_response(fd, 200);
    http_send_header(fd, "Content-Type", http_get_mime_type(complete_path));
    http_send_header(fd, "Content-Length", file_size_string);
    http_end_headers(fd);

    /* read and send file */
    char *buffer = (char *) malloc(READ_SIZE);
    if (!buffer) {
      fprintf(stderr, "Malloc Failed\n");
      exit(ENOBUFS);
    }
    int total_read = 0;
    int read_amount;
    while (total_read < file_size) {
      read_amount = fread(buffer, 1, READ_SIZE, file);
      if (read_amount == -1) break;
      http_send_data(fd, buffer, read_amount);
      total_read += read_amount;
    }
    fclose(file);
    free(buffer);
  } else {
    fprintf(stderr, "Failed to open file: %s\n", complete_path);
    http_send_error_response(fd, 500);
    return;
  }
}

/*
 * Serves the contents of the directory at `path` to the client socket `fd`.
 * If the directory contain an index.html file serves contents if the file.
 * Otherwise serves a list of directory's files.
 * It is the caller's reponsibility to ensure that the file stored at `path` exists.
 */
void serve_directory(int fd, char *path, char *complete_path) {
  /* create index.html path */
  char *index_path = (char *) malloc(strlen(complete_path) + 11 + 1);
  memcpy(index_path, complete_path, strlen(complete_path));
  memcpy(index_path + strlen(complete_path), "/index.html", 11 + 1);

  /* respond with index.html if it exists */
  if (access(index_path, F_OK) == 0) {
    serve_file(fd, index_path);
    free(index_path);
    return;
  }
  free(index_path);

  /* respond with list of children */
  DIR *directory;
  struct dirent *dir;
  directory = opendir(complete_path);
  if (directory) {
    /* create response body */
    char *response_body = (char *) malloc(MAX_BODY_SIZE);
    if (!response_body) {
      fprintf(stderr, "Malloc Failed\n");
      exit(ENOBUFS);
    }
    char *current = response_body;
    while ((dir = readdir(directory)) != NULL) {
      if (dir->d_name[0] == '.') continue;
      if (current - response_body > MAX_BODY_SIZE - strlen(path) - 2 * strlen(dir->d_name) - 64) {
        current += sprintf(current, "<span>...</span>\n");
        break;
      }
      current += sprintf(current,
        "<a href=\"http://192.168.162.162:8000%s/%s\">%s</a><br>\n",
        path, dir->d_name, dir->d_name);
    }
    closedir(directory);

    /* send response */
    http_start_response(fd, 200);
    http_send_header(fd, "Content-Type", http_get_mime_type(".html"));
    http_end_headers(fd);
    http_send_string(fd, response_body);

    /* free resources */
    free(response_body);
  } else {
    fprintf(stderr, "Failed to open directory: %s\n", complete_path);
    http_send_error_response(fd, 500);
    return;
  }
}


/*
 * Reads an HTTP request from stream (fd), and writes an HTTP response
 * containing:
 *
 *   1) If user requested an existing file, respond with the file
 *   2) If user requested a directory and index.html exists in the directory,
 *      send the index.html file.
 *   3) If user requested a directory and index.html doesn't exist, send a list
 *      of files in the directory with links to each.
 *   4) Send a 404 Not Found response.
 * 
 *   Closes the client socket (fd) when finished.
 */
void handle_files_request(int fd) {
  struct http_request *request = http_request_parse(fd);

  if (request == NULL || request->path[0] != '/') {
    http_send_error_response(fd, 400);
    close(fd);
    return;
  }

  if (strstr(request->path, "..") != NULL) {
    http_send_error_response(fd, 403);
    close(fd);
    return;
  }

  char *complete_path = get_complete_path(request->path);
  struct stat path_stat;
  if (stat(complete_path, &path_stat) != 0) {
    http_send_error_response(fd, 404);
  } else {
    if (S_ISREG(path_stat.st_mode)) {
      serve_file(fd, complete_path);
    } else if (S_ISDIR(path_stat.st_mode)) {
      serve_directory(fd, request->path, complete_path);
    } else {
      http_send_error_response(fd, 404);
    }
  }

  free(complete_path);
  return;
}


/*
 * Relays traffic from src_fd to dest_fd given in proxy_details.
 */
void relay_traffic(proxy_details_t *proxy_details) {
  char *buffer = (char *) malloc(READ_SIZE);
  ssize_t read_write_amount;
  while (!*proxy_details->done && (read_write_amount = read(proxy_details->src_fd, buffer, READ_SIZE)) > 0) {
    http_send_data(proxy_details->dest_fd, buffer, read_write_amount);
  }
  free(buffer);

  *proxy_details->done = 1;
  if (proxy_details->cond_var != NULL) {
    pthread_cond_signal(proxy_details->cond_var);
  }
}

/* Wrapper for relay_traffic function */
void *relay_traffic_wrapper(void *args) {
  proxy_details_t *proxy_details = (proxy_details_t *) args;
  relay_traffic(proxy_details);
  return NULL;
}

/*
 * Does DNS lookup of server_proxy_hostname and opens a connection to it.
 */
int open_proxy_target(int fd) {
  struct sockaddr_in target_address;
  memset(&target_address, 0, sizeof(target_address));
  target_address.sin_family = AF_INET;
  target_address.sin_port = htons(server_proxy_port);

  struct hostent *target_dns_entry = gethostbyname2(server_proxy_hostname, AF_INET);

  int target_fd = socket(PF_INET, SOCK_STREAM, 0);
  if (target_fd == -1) {
    fprintf(stderr, "Failed to create a new socket: error %d: %s\n", errno, strerror(errno));
    close(fd);
    exit(errno);
  }

  if (target_dns_entry == NULL) {
    fprintf(stderr, "Cannot find host: %s\n", server_proxy_hostname);
    close(target_fd);
    close(fd);
    exit(ENXIO);
  }

  char *dns_address = target_dns_entry->h_addr_list[0];

  memcpy(&target_address.sin_addr, dns_address, sizeof(target_address.sin_addr));
  int connection_status = connect(target_fd, (struct sockaddr*) &target_address,
      sizeof(target_address));

  if (connection_status < 0) {
    /* Dummy request parsing, just to be compliant. */
    http_request_parse(fd);

    http_send_error_response(fd, 502);
    close(target_fd);
    close(fd);
    return -1;
  }

  return target_fd;
}

/*
 * Opens a connection to the proxy target (hostname=server_proxy_hostname and
 * port=server_proxy_port) and relays traffic to/from the stream fd and the
 * proxy target. HTTP requests from the client (fd) should be sent to the
 * proxy target, and HTTP responses from the proxy target should be sent to
 * the client (fd).
 *
 *   +--------+     +------------+     +--------------+
 *   | client | <-> | httpserver | <-> | proxy target |
 *   +--------+     +------------+     +--------------+
 */
void handle_proxy_request(int fd) {
  /* initialize arguments for creating threads */
  int target_fd = open_proxy_target(fd);
  if (target_fd == -1) return;
  int done = 0;
  pthread_cond_t cond_var;
  pthread_cond_init(&cond_var, NULL);

  proxy_details_t *sender_proxy_details = malloc(sizeof(proxy_details_t));
  if (!sender_proxy_details) {
    fprintf(stderr, "Malloc Failed\n");
    exit(ENOBUFS);
  }
  sender_proxy_details->src_fd = fd;
  sender_proxy_details->dest_fd = target_fd;
  sender_proxy_details->done = &done;
  sender_proxy_details->cond_var = &cond_var;

  proxy_details_t *receiver_proxy_details = malloc(sizeof(proxy_details_t));
  if (!receiver_proxy_details) {
    fprintf(stderr, "Malloc Failed\n");
    exit(ENOBUFS);
  }
  receiver_proxy_details->src_fd = target_fd;
  receiver_proxy_details->dest_fd = fd;
  receiver_proxy_details->done = &done;
  receiver_proxy_details->cond_var = &cond_var;

  /* create threads */
  pthread_t sender_thread, receiver_thread;
  pthread_create(&sender_thread, NULL, relay_traffic_wrapper, sender_proxy_details);
  pthread_create(&receiver_thread, NULL, relay_traffic_wrapper, receiver_proxy_details);

  /* wait for threads to finish */
  pthread_mutex_t mutex;
  pthread_mutex_init(&mutex, NULL);
  while (!done) {
    pthread_cond_wait(&cond_var, &mutex);
  }

  /* free resources */
  pthread_cancel(sender_thread);
  pthread_cancel(receiver_thread);
  pthread_cond_destroy(&cond_var);
  pthread_mutex_destroy(&mutex);
  free(sender_proxy_details);
  free(receiver_proxy_details);
  close(target_fd);
}


/* Waits for a work and runs it */
void work(void (*request_handler)(int)) {
  while (1) {
    int client_socket_fd = wq_pop(&work_queue);
    request_handler(client_socket_fd);
    close(client_socket_fd);
  }
}

/* Wrapper for work function */
void *work_wrapper(void *args) {
  void (*request_handler)(int) = (void (*)(int)) args;
  work(request_handler);
  return NULL;
}

/* Creates (num_threads) threads to run works */
void init_thread_pool(int num_threads, void (*request_handler)(int)) {
  pthread_t threads[num_threads];
  for (int i = 0; i < num_threads; i++) {
    pthread_create(&threads[i], NULL, work_wrapper, request_handler);
  }
}

/*
 * Opens a TCP stream socket on all interfaces with port number PORTNO. Saves
 * the fd number of the server socket in *socket_number. For each accepted
 * connection, calls request_handler with the accepted fd number.
 */
void serve_forever(int *socket_number, void (*request_handler)(int)) {
  struct sockaddr_in server_address, client_address;
  size_t client_address_length = sizeof(client_address);
  int client_socket_number;

  *socket_number = socket(PF_INET, SOCK_STREAM, 0);
  if (*socket_number == -1) {
    perror("Failed to create a new socket");
    exit(errno);
  }

  int socket_option = 1;
  if (setsockopt(*socket_number, SOL_SOCKET, SO_REUSEADDR, &socket_option,
        sizeof(socket_option)) == -1) {
    perror("Failed to set socket options");
    exit(errno);
  }

  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(server_port);

  if (bind(*socket_number, (struct sockaddr *) &server_address,
        sizeof(server_address)) == -1) {
    perror("Failed to bind on socket");
    exit(errno);
  }

  if (listen(*socket_number, 1024) == -1) {
    perror("Failed to listen on socket");
    exit(errno);
  }

  printf("Listening on port %d...\n", server_port);

  wq_init(&work_queue);
  init_thread_pool(num_threads, request_handler);

  while (1) {
    client_socket_number = accept(*socket_number,
        (struct sockaddr *) &client_address,
        (socklen_t *) &client_address_length);
    if (client_socket_number < 0) {
      perror("Error accepting socket");
      continue;
    }

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);

    wq_push(&work_queue, client_socket_number);

    printf("Accepted connection from %s on port %d\n",
        inet_ntoa(client_address.sin_addr),
        client_address.sin_port);
  }

  shutdown(*socket_number, SHUT_RDWR);
  close(*socket_number);
}

int server_fd;
void signal_callback_handler(int signum) {
  printf("Caught signal %d: %s\n", signum, strsignal(signum));
  printf("Closing socket %d\n", server_fd);
  if (close(server_fd) < 0) perror("Failed to close server_fd (ignoring)\n");
  exit(0);
}

char *USAGE =
  "Usage: ./httpserver --files www_directory/ --port 8000 [--num-threads 5]\n"
  "       ./httpserver --proxy inst.eecs.berkeley.edu:80 --port 8000 [--num-threads 5]\n";

void exit_with_usage() {
  fprintf(stderr, "%s", USAGE);
  exit(EXIT_SUCCESS);
}

int main(int argc, char **argv) {
  signal(SIGINT, signal_callback_handler);
  signal(SIGPIPE, SIG_IGN);

  /* Default settings */
  num_threads = 5;
  server_port = 8000;
  void (*request_handler)(int) = NULL;

  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp("--files", argv[i]) == 0) {
      request_handler = handle_files_request;
      free(server_files_directory);
      server_files_directory = argv[++i];
      if (!server_files_directory) {
        fprintf(stderr, "Expected argument after --files\n");
        exit_with_usage();
      }
    } else if (strcmp("--proxy", argv[i]) == 0) {
      request_handler = handle_proxy_request;

      char *proxy_target = argv[++i];
      if (!proxy_target) {
        fprintf(stderr, "Expected argument after --proxy\n");
        exit_with_usage();
      }

      char *colon_pointer = strchr(proxy_target, ':');
      if (colon_pointer != NULL) {
        *colon_pointer = '\0';
        server_proxy_hostname = proxy_target;
        server_proxy_port = atoi(colon_pointer + 1);
      } else {
        server_proxy_hostname = proxy_target;
        server_proxy_port = 80;
      }
    } else if (strcmp("--port", argv[i]) == 0) {
      char *server_port_string = argv[++i];
      if (!server_port_string) {
        fprintf(stderr, "Expected argument after --port\n");
        exit_with_usage();
      }
      server_port = atoi(server_port_string);
    } else if (strcmp("--num-threads", argv[i]) == 0) {
      char *num_threads_str = argv[++i];
      if (!num_threads_str || (num_threads = atoi(num_threads_str)) < 1) {
        fprintf(stderr, "Expected positive integer after --num-threads\n");
        exit_with_usage();
      }
    } else if (strcmp("--help", argv[i]) == 0) {
      exit_with_usage();
    } else {
      fprintf(stderr, "Unrecognized option: %s\n", argv[i]);
      exit_with_usage();
    }
  }

  if (server_files_directory == NULL && server_proxy_hostname == NULL) {
    fprintf(stderr, "Please specify either \"--files [DIRECTORY]\" or \n"
                    "                      \"--proxy [HOSTNAME:PORT]\"\n");
    exit_with_usage();
  }

  serve_forever(&server_fd, request_handler);

  return EXIT_SUCCESS;
}
