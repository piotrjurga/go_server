#include "protocol.h"

void send_request(int connection, Request r) {
    write(connection, &r, sizeof(Request));
}

struct ThreadData {
    int connection;
    Request request;
};

void *thread_send_request(void *t_data) {
    pthread_detach(pthread_self());
    ThreadData *th_data = (ThreadData *)t_data;
    write(th_data->connection, &th_data->request, sizeof(Request));
    free(th_data);
    pthread_exit(0);
}

void send_request_async(int connection, Request r) {
    pthread_t thread;
    ThreadData *thread_data = (ThreadData *)malloc(sizeof(ThreadData));
    thread_data->connection = connection;
    thread_data->request = r;
    pthread_create(&thread, 0, thread_send_request, (void *)thread_data);
}
