#ifndef GTIPC_SERVER_H
#define GTIPC_SERVER_H

#include <unistd.h>
#include <mqueue.h>
#include <pthread.h>

/**
 * Describes a single client as seen by the server.
 *
 * Note: **inverted** use of send and recv i.e. send is from server to client
 */
typedef struct __client {
    pid_t pid;

    // Queues
    mqd_t send_queue;
    mqd_t recv_queue;

    // Shared memory
    char *shm_addr;
    size_t shm_size;
    char shm_name[100];

    // Reference to client's thread handler
    pthread_t client_thread;
    int stop_client_thread;
} client;

/**
 * Doubly-linked list of clients.
 */
typedef struct __client_node {
    client client;
    struct __client_node *tail;
    struct __client_node *next;
    struct __client_node *prev;
} client_list;

/**
 * Client background thread arguments.
 */
typedef struct __client_thread_arg {
    gtipc_request req;
    client *client;
} client_thread_arg;

/* Client register functions */
int register_client(gtipc_registry *reg);
int unregister_client(int pid, int close);

/* POSIX IPC setup and cleanup */
void open_shm_object(gtipc_registry *reg, client *client);
void resize_shm_object(client *client);

/* Internal functions */
void init_server();
void exit_server();

/* Server API functions */
void add(gtipc_arg *arg);
void mul(gtipc_arg *arg);
void *compute_service(void *data);

#endif
