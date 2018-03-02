#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>

#include "gtipc/messages.h"
#include "gtipc/params.h"

#include "server.h"

/* Global state */
// Global registry queue
mqd_t global_registry;

// Registry thread
pthread_t registry_thread;
static int STOP_REGISTRY = 0; // Flag to stop registry thread

// Doubly linked list of registered clients
client_list *clients = NULL;

// Service thread management
static int MAX_THREADS = 100;

struct __service_threads {
    int curr;
    pthread_mutex_t curr_mutex;
    pthread_cond_t curr_cond;
} service_threads;

void exit_error(char *msg) {
    fprintf(stderr, "%s", msg);
    exit(EXIT_FAILURE);
}

void add(gtipc_arg *arg) {
    arg->res = arg->x + arg->y;
}

void mul(gtipc_arg *arg) {
    arg->res = arg->x * arg->y;
}

/**
 * Mark a request as complete in shared memory
 * @param entry Pointer to completed entry
 */
void request_complete(gtipc_shared_entry *entry, gtipc_arg *arg) {
    pthread_mutex_t *mutex = &entry->mutex;
    pthread_mutex_lock(mutex);
    entry->done = 1;
    entry->arg = *arg;
    pthread_mutex_unlock(mutex);
}

void cleanup_compute_thread(client_thread_arg *thread_arg) {
    // Decrement number of threads and signal condition
    pthread_mutex_lock(&service_threads.curr_mutex);
    service_threads.curr--;
    pthread_cond_signal(&service_threads.curr_cond);
    pthread_mutex_unlock(&service_threads.curr_mutex);

    // Free allocated thread arg
    free(thread_arg);
}

void spawn_compute_thread(gtipc_request *req, client *client) {
    // Spawn a compute background thread once one is available (see: MAX_THREADS)
    pthread_mutex_lock(&service_threads.curr_mutex);
    while (service_threads.curr >= MAX_THREADS)
        pthread_cond_wait(&service_threads.curr_cond, &service_threads.curr_mutex);

    // Increment running thread counter
    service_threads.curr++;

    pthread_mutex_unlock(&service_threads.curr_mutex);

    // Allocate a new thread arg on heap
    client_thread_arg *thread_arg = malloc(sizeof(client_thread_arg));
    thread_arg->req = *req;
    thread_arg->client = client;

    // Spawn a new thread to serve the request
    pthread_t thread;
    pthread_create(&thread, NULL, compute_service, (void *)thread_arg);
}

/**
 * Compute service thread for a single request.
 */
void *compute_service(void *data) {
    client_thread_arg *thread_arg = (client_thread_arg *)data;

    // Extract current client, request, and argument
    client *client = thread_arg->client;
    gtipc_request *req = &thread_arg->req;
    gtipc_arg *arg = &req->arg;
    gtipc_shared_entry *entry = (gtipc_shared_entry *)(client->shm_addr + req->entry_idx * sizeof(gtipc_shared_entry));

    // Perform computation based on requested service
    switch (req->service) {
        case GTIPC_ADD:
            add(arg);
            break;
        case GTIPC_MUL:
            mul(arg);
            break;
        default:
            fprintf(stderr, "ERROR: Invalid service requested by client %d\n", req->pid);
    }

    // Mark entry in shared memory as DONE (i.e., request has been served)
    request_complete(entry, arg);

    cleanup_compute_thread(thread_arg);

    printf("Client %d, request %d: done = 1\n", client->pid, req->request_id);

    return NULL;
}

/**
 * Client thread handler. Each client gets its own thread for queue management.
 */
static void *client_handler(void *node) {
    // Get current thread's client object, as passed in
    client *client = &((client_list *)node)->client;

    // Buffer for incoming gtipc_request
    char buf[sizeof(gtipc_request)];
    gtipc_request *req;

    // Timeout parameter for message queue operations
    struct timespec ts;

    while (!client->stop_client_thread) {
        // Setup a 10 ms timeout
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;
        ts.tv_sec = 0;

        // Wait for message on receive queue
        ssize_t received = mq_timedreceive(client->recv_queue, buf, sizeof(gtipc_request), NULL, &ts);

        // If receive error, ignore the request (TODO: Is this sound behavior?)
        if (received != -1) {
            // Extract request and argument
            req = (gtipc_request *)buf;

            if (req->request_id == -1) {
                // Resize request received from client
                resize_shm_object(client);
            }

            else {
                // Spawn background thread for computation
                spawn_compute_thread(req, client);
            }
        }
    }
}

/* Registry thread handler. */
static void *registry_handler(void *unused) {
    char recv_buf[sizeof(gtipc_registry)];
    gtipc_registry *reg;

    // Timeout parameter for message queue operations
    struct timespec ts;

    while (!STOP_REGISTRY) {
        // Setup a 10 ms timeout
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 10000000;
        ts.tv_sec = 0;

        // Wait for a new registry message from client (blocking)
        mq_receive(global_registry, recv_buf, sizeof(gtipc_registry), NULL);

        // Read out registry entry
        reg = (gtipc_registry *)recv_buf;

        #if DEBUG
        printf("CMD: %d, PID: %ld, Send queue: %s, Recv queue: %s\n", reg->cmd, (long)reg->pid,
               reg->send_queue_name, reg->recv_queue_name);
        #endif

        // Register or unregister the client based on given command
        switch (reg->cmd) {
            case GTIPC_CLIENT_REGISTER:
                register_client(reg);
                break;
            case GTIPC_CLIENT_UNREGISTER:
            case GTIPC_CLIENT_CLOSE:
                unregister_client(reg->pid, 0);
                break;
            default:
                fprintf(stderr, "ERROR: Incorrect registry command received from client %d\n", reg->pid);
        }
    }
}

/* Given a PID, return client object */
client_list *find_client(int pid) {
    client_list *list = clients;

    while (list != NULL && list->client.pid != pid) {
        list = list->next;
    }

    return list;
}

/* Remove a client from the given clients list */
void remove_client(client_list *node) {
    if (node != NULL) {
        // If current is head AND tail, empty the list
        if (node->prev == NULL && node->next == NULL)
            clients = NULL;
        else {
            // If current is not head node, update previous node
            if (node->prev != NULL)
                node->prev->next = node->next;

            // If current is not tail node, update next
            if (node->next != NULL)
                node->next->prev = node->prev;
            else {
                // Removing tail node, so update global tail
                clients->tail = node->prev;
            }
        }
    }
}

/* Insert a client at TAIL of global clients list */
void append_client(client_list *node) {
    if (clients == NULL) {
        // Initialize the list
        node->tail = node;
        clients = node;
    }
    else {
        // Get tail of list
        client_list *tail = clients->tail;

        // Insert new node after tail
        node->prev = tail;
        node->next = NULL;
        node->tail = node;

        // Set old tail's next to new node
        tail->next = node;

        // Update global tail
        clients->tail = node;
    }
}

/**
 * Open a shared memory object based on given name.
 * @param reg
 * @param client
 */
void open_shm_object(gtipc_registry *reg, client *client) {
    // Map shared memory
    int fd;

    if ((fd = shm_open(reg->shm_name, O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        fprintf(stderr, "ERROR: Failed to open shared mem object for client %d\n", client->pid);
        exit(EXIT_FAILURE);
    }

    memcpy(client->shm_name, reg->shm_name, 100);

    // Determine size of shared mem object
    struct stat s;
    fstat(fd, &s);
    client->shm_size = (size_t)s.st_size;

    // Map shared memory based on size determined
    client->shm_addr = mmap(NULL, client->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    close(fd);
}

/**
 * Resize the shared mem object in coordination with client.
 */
void resize_shm_object(client *client) {
    int fd;

    // Open new shared memory object
    if ((fd = shm_open(client->shm_name, O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        fprintf(stderr, "ERROR: Failed to resize shared mem object for client %d\n", client->pid);
        exit(EXIT_FAILURE);
    }

    // Determine size of new shared mem object
    struct stat s;
    fstat(fd, &s);
    size_t new_shm_size = (size_t)s.st_size;

    // Map new shared memory segment
    char *new_shm_addr = mmap(NULL, new_shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    // Wait for all in-progress client requests to complete
    pthread_mutex_lock(&service_threads.curr_mutex);
    while (service_threads.curr != 0)
        pthread_cond_wait(&service_threads.curr_cond, &service_threads.curr_mutex);
    pthread_mutex_unlock(&service_threads.curr_mutex);

    // Copy over old shared segment to new segment
    memcpy(new_shm_addr, client->shm_addr, client->shm_size);

    // Send notification to client that new segment is ready
    gtipc_response resp;
    resp.request_id = -1;
    mq_send(client->send_queue, (char *)&resp, sizeof(gtipc_response), 1);

    // Switch shared memory refs
    client->shm_addr = new_shm_addr;
    client->shm_size = new_shm_size;

    close(fd);
}

/**
 * Given an incoming registry object, create a client
 * and add it to the clients list.
 */
int register_client(gtipc_registry *reg) {
    // Create new client object based on given registry
    client client;

    client.pid = reg->pid;

    // Open queues
    client.send_queue = mq_open(reg->recv_queue_name, O_RDWR);
    client.recv_queue = mq_open(reg->send_queue_name, O_RDWR);

    // Check for message queue errors
    if (client.send_queue == (mqd_t)-1 || client.recv_queue == (mqd_t)-1) {
        fprintf(stderr, "ERROR (%d): Client %d send and/or receive queue(s) failed to open\n", errno, client.pid);
        exit(EXIT_FAILURE);
    }

    // Open the shared mem object
    open_shm_object(reg, &client);

    // Append newly created client to global list
    client_list *node = malloc(sizeof(client_list));
    node->client = client;
    append_client(node);

    // Spin up client's background handler thread
    client.stop_client_thread = 0;
    pthread_create(&client.client_thread, NULL, client_handler, (void *)node);

    #if DEBUG
        printf("Client %d has queues %d and %d\n", clients->client.pid, clients->client.send_queue, clients->client.recv_queue);
    #endif

    return 0;
}

int unregister_client(int pid, int close) {
    client_list *node = find_client(pid);

    if (node == NULL)
        return 0;

    // Get ref to current client object
    client *client = &node->client;

    // Send poison pill to client iff server is closing (i.e. close == 1)
    if (close) {
        // Create poison pill
        gtipc_registry registry;
        registry.cmd = GTIPC_SERVER_CLOSE;

        // Send poison pill to client
        // TODO: FIX THIS SO THAT SEND QUEUE RECEIVE REGISTRY MESSAGES
        if (mq_send(client->send_queue, (char *)&registry, sizeof(gtipc_registry), 1)) {
            fprintf(stderr, "ERROR: Could not send poison pill message to client %d\n", client->pid);
        }
    }

    // Remove current client from list
    remove_client(node);

    // Free up resources used by client
    // Close queues
    mq_close(client->send_queue);
    mq_close(client->recv_queue);

    // Unmap memory
    munmap(client->shm_addr, client->shm_size);

    // Stop client handler thread
    client->stop_client_thread = 1;
//    pthread_cancel(client->client_thread);

    free(node);

    return 0;
}

void exit_server() {
    // Perform client cleanup
    client_list *list = clients;

    while (list != NULL) {
        unregister_client(list->client.pid, 1);
        list = list->next;
    }

    // Close and unlink registry queue
    if (global_registry) {
        mq_close(global_registry);
        mq_unlink(GTIPC_REGISTRY_QUEUE);
    }

    // Stop registry thread
    STOP_REGISTRY = 1;
}

void init_server() {
    // Set exit handler for cleanup
    atexit(exit_server);

    // Set queue attrs first to fix message size
    struct mq_attr attr;
    attr.mq_msgsize = sizeof(gtipc_registry);
    attr.mq_maxmsg = 10; // NOTE: must be <=10 for *unprivileged* process

    // Create the global registry queue
    global_registry = mq_open(GTIPC_REGISTRY_QUEUE, O_EXCL | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR, &attr);

    if (global_registry == (mqd_t)-1) {
        // Already exists => unlink the queue first, then re-create
        if (errno == EEXIST) {
            mq_unlink(GTIPC_REGISTRY_QUEUE);
            global_registry = mq_open(GTIPC_REGISTRY_QUEUE, O_EXCL | O_CREAT | O_RDWR, S_IRUSR | S_IWUSR, &attr);
        }

        else
            exit_error("FATAL: Unable to create global registry\n");
    }

    // Spawn background thread for registry queue handling
    if (pthread_create(&registry_thread, NULL, registry_handler, NULL)) {
        exit_error("FATAL: Failed to create registry background thread!\n");
    }

    // Initialize number of service threads to 0
    service_threads.curr = 0;
}

int main(int argc, char **argv) {
    init_server();

    char in[2];

    // Wait for user exit
    while (in[0] != 'x') fgets(in, 2, stdin);

    exit_server();

    return 0;
}
