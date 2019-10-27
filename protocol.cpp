#include "protocol.h"

int read_size(int connection, void *data, size_t size) {
    size_t bytes_read = 0;
    while(bytes_read < size) {
        int bytes = read(connection, data, size - bytes_read);
        if(bytes == -1 || bytes == 0) return -1;
        bytes_read += (size_t)bytes;
    }
    return 0;
}

int write_size(int connection, void *data, size_t size) {
    size_t bytes_written = 0;
    while(bytes_written < size) {
        int bytes = write(connection, data, size - bytes_written);
        if(bytes == -1) return -1;
        bytes_written += (size_t)bytes;
    }
    return 0;
}

int write_size(Connection *connection, void *data, size_t size) {
    puts("writing size");
    pthread_mutex_lock(&connection->mutex);
    int res = write_size(connection->desc, data, size);
    pthread_mutex_unlock(&connection->mutex);
    puts("written size");
    return res;
}
