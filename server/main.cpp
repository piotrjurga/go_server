#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <assert.h>
#include <vector>
#include <sys/mman.h>

#include "game_logic.h"
#include "protocol.h"

#define Kilobytes(x) (1024*(x))
#define Megabytes(x) (1024*Kilobytes(x))
#define Gigabytes(x) (1024*Megabytes(x))

#define SERVER_PORT 1234
#define QUEUE_SIZE 5

template <class T>
struct SyncDynamicArray {
    int32_t size;
    int32_t allocated;
    T *data;
    pthread_mutex_t mutex;

    SyncDynamicArray() {
        size = 0;
        allocated = 0;
        data = (T *)mmap(0, Gigabytes(2l), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(data == (T *)-1) {
            printf("%s\n", strerror(errno));
            exit(1);
        }
    }

    T& operator[](uint64_t i) { return data[i]; }
    T& operator[](int i)      { return data[i]; }


    int push(T el) {
        while(allocated <= (size+1)*(int32_t)sizeof(T)) {
            allocated += Kilobytes(4);
            int err = mprotect(data, allocated, PROT_READ | PROT_WRITE);
            if(err) {
                printf("%s\n", strerror(errno));
                assert(false);
            }
        }
        data[size] = el;
        size += 1;
        return (int)size-1;
    }

    int push_lock(T el) {
        pthread_mutex_lock(&mutex);
        int index = push(el);
        pthread_mutex_unlock(&mutex);
        return index;
    }

    T pop_no_lock() {
        T res = data[--size];
        memset((void *)&data[size], 0, sizeof(T));

        if(allocated >= size*sizeof(T) + Kilobytes(8)) {
            int err = mprotect(data, allocated, PROT_NONE);
            if(err) {
                printf("Error in mprotect: %s\n", strerror(errno));
                assert(false);
            }
            allocated -= Kilobytes(4);
            err = mprotect(data, allocated, PROT_READ | PROT_WRITE);
            if(err) {
                printf("Error in mprotect: %s\n", strerror(errno));
                assert(false);
            }
        }

        return res;
    }

    T pop() {
        pthread_mutex_lock(&mutex);
        T res = pop_no_lock();
        pthread_mutex_unlock(&mutex);
        return res;
    }
};


struct Room {
    GameData game;
    int32_t player_a;
    int32_t player_b;
    char name[16];
};

// first valid room index is 1
static SyncDynamicArray<Room> rooms;
// first valid client index is 1
static SyncDynamicArray<Connection> clients;

int first_empty_slot(SyncDynamicArray<Room> &arr) {
    pthread_mutex_lock(&arr.mutex);
    int32_t i = 0;
    for(; i < arr.size; i++) {
        if(arr[i].player_a == 0)
            break;
    }
    if(i == arr.size) {
        Room fill = {};
        arr.push(fill);
    }
    arr[i].player_a = -1;
    pthread_mutex_unlock(&arr.mutex);
    return i;
}

int first_empty_slot(SyncDynamicArray<Connection> &arr) {
    pthread_mutex_lock(&arr.mutex);
    int32_t i = 0;
    for(; i < arr.size; i++) {
        if(arr[i].desc == 0)
            break;
    }
    if(i == arr.size) {
        Connection fill = {};
        arr.push(fill);
    }
    arr[i].desc = -1;
    pthread_mutex_unlock(&arr.mutex);
    return i;
}

struct ThreadData {
    int client_index;
};

void *handle_client(void *t_data) {
    pthread_detach(pthread_self());
    ThreadData *th_data = (ThreadData *)t_data;
    int client_index = th_data->client_index;
    Connection *connection = &clients[client_index];
    printf("starting thread for %d\n", client_index);

    int32_t active_room_id = 0;

    bool done = false;
    while(!done) {
        Request req = {};
        Response res = {};
        int err = read_struct(connection->desc, &req);
        if(err) { done = true; break; }
        switch(req.type) {
            case REQUEST_NEW_ROOM: {
                printf("requested new room by connection %d\n", client_index);
                int32_t new_room_id = 0;
                int board_size = req.new_room.board_size;
                printf("requested board size %d\n", board_size);
                res.type = RESPONSE_NEW_ROOM_RESULT;
                if(active_room_id != 0 || board_size < 2 || board_size > 19) {
                    res.new_room_result.room_id = 0;
                    int err = write_struct(connection, &res);
                    if(err) { done = true; break; }
                    break;
                }

                Room new_room = {};
                new_room.game.board.size = board_size;
                new_room.player_a = client_index;
                memcpy(new_room.name, req.new_room.name, 16);

                new_room_id = first_empty_slot(rooms);
                active_room_id = new_room_id;
                rooms[new_room_id] = new_room;

                res.new_room_result.room_id = new_room_id;
                int err = write_struct(connection, &res);
                if(err) { done = true; break; }
                printf("new room id: %d\n", new_room_id);
            } break;

            case REQUEST_JOIN_ROOM: {
                res.type = RESPONSE_JOIN_RESULT;
                int32_t room_id = req.join_room.room_id;
                printf("reqested join id %d by connection %d\n", room_id, client_index);

                res.join_result.success = false;
                if(active_room_id != 0) {
                    int err = write_struct(connection, &res);
                    if(err) done = true;
                    break;
                }

                if(room_id < 0 || room_id >= (int32_t)rooms.size || rooms[room_id].player_b != 0) {
                    int err = write_struct(connection, &res);
                    if(err) done = true;
                    break;
                }

                rooms[room_id].player_b = client_index;
                int other_player = rooms[room_id].player_a;
                active_room_id = room_id;

                res.join_result.success = true;
                int err = write_struct(connection, &res);
                if(err) { done = true; break; }

                Response res2 = {};
                res2.type = RESPONSE_PLAYER_JOINED;
                err = write_struct(&clients[other_player], &res2);
                if(err) { done = true; break; }
                puts("join success");
             } break;

            case REQUEST_LEAVE_ROOM: {
                puts("got request leave room");
                if(active_room_id != 0) {
                    int other_player = rooms[active_room_id].player_a;
                    if(other_player == client_index) {
                        other_player = rooms[active_room_id].player_b;
                    }
                    if(other_player) {
                        Response res = {};
                        res.type = RESPONSE_EXIT;
                        write_struct(&clients[other_player], &res);
                    }
                    rooms[active_room_id] = {};
                }
                active_room_id = 0;
            } break;

            case REQUEST_MAKE_MOVE: {
                v2_8 move = req.make_move.move;
                int x = (int)move.x, y = (int)move.y;
                printf("reqested make move (%d, %d) by connection %d\n", x, y, client_index);
                if(rooms[active_room_id].game.board.size == 0) {
                    // if the active game has no size, we know for sure
                    // the other player has left it and reset it's state
                    // so any further moves are impossible
                    active_room_id = 0;
                    break;
                }

                bool result = rooms[active_room_id].game.maybe_make_move(x, y);
                if(!result) {
                    pthread_mutex_lock(&connection->mutex);
                    res.type = RESPONSE_ILLEGAL_MOVE;
                    int err = write_struct(connection->desc, &res);
                    // send back actual game data to assure it is
                    // the same as the client game data
                    auto game_data = &rooms[active_room_id].game;
                    err |= write_struct(connection->desc, game_data);
                    pthread_mutex_unlock(&connection->mutex);
                    if(err) { done = true; break; }
                }

                int other_player = rooms[active_room_id].player_a;
                if(other_player == client_index)
                    other_player = rooms[active_room_id].player_b;
                // TODO(piotr): broadcast this message to everyone
                // who is watching the game
                printf("sending move to player %d\n", other_player);
                res.type = RESPONSE_NEW_MOVE;
                res.new_move.room_id = active_room_id;
                res.new_move.move.x = x;
                res.new_move.move.y = y;

                err = write_struct(clients[other_player].desc, &res);
                if(err) { done = true; break; }
            } break;

            case REQUEST_LIST_ROOMS: {
                printf("got request list rooms from %d\n", client_index);
                pthread_mutex_lock(&rooms.mutex);
                pthread_mutex_lock(&connection->mutex);
                res.type = RESPONSE_LIST_ROOMS;
                int valid_room_count = 0;
                for(int i = 1; i < rooms.size; i++)
                    if(rooms[i].player_a)
                        valid_room_count++;
                res.list_rooms.size = valid_room_count;
                int err = write_struct(connection->desc, &res);
                if(err) done = true;
                else {
                    for(int i = 1; i < rooms.size; i++) {
                        if(rooms[i].player_a == 0) continue;
                        int err = write_struct(connection->desc, &i);
                        if(err) { done = true; break; }
                        err = write_size(connection->desc, rooms[i].name, 16);
                        if(err) { done = true; break; }
                        bool can_join = (rooms[i].player_b == 0);
                        err = write_struct(connection->desc, &can_join);
                        if(err) { done = true; break; }
                        err = write_struct(connection->desc, &rooms[i].game.board);
                        if(err) { done = true; break; }
                    }
                }
                pthread_mutex_unlock(&connection->mutex);
                pthread_mutex_unlock(&rooms.mutex);
            } break;

            case REQUEST_NONE: {
                printf("got request none from %d\n", client_index);
                done = true;
            } break;
            case REQUEST_EXIT: {
                printf("got reqest exit from %d\n", client_index);
                done = true;
            } break;

            default: {
                printf("received unrecognized request type %d\n", (int)req.type);
            }
        }
    }

    if(active_room_id != 0) {
        int other_player = rooms[active_room_id].player_a;
        if(rooms[active_room_id].player_a == client_index) {
            other_player = rooms[active_room_id].player_b;
        }
        if(other_player) {
            Response res = {};
            res.type = RESPONSE_EXIT;
            write_struct(&clients[other_player], &res);
        }
        rooms[active_room_id] = {};
    }
    printf("ending thread for %d\n", client_index);
    connection->desc = 0;
    pthread_mutex_destroy(&connection->mutex);
    free(th_data);
    pthread_exit(0);
}

void handle_connection(int connection_socket_descriptor) {
    int create_result = 0;

    pthread_t thread1;

    Connection c = {};
    c.desc = connection_socket_descriptor;
    int client_index = first_empty_slot(clients);
    clients[client_index] = c;

    ThreadData *t_data = (ThreadData *)malloc(sizeof(ThreadData));
    t_data->client_index = client_index;

    create_result = pthread_create(&thread1, NULL, handle_client, (void *)t_data);
    if (create_result) {
        printf("Błąd przy próbie utworzenia wątku, kod błędu: %d\n", create_result);
        exit(-1);
    }
}

int main(int argc, char **argv) {
    Connection invalid_connection = {};
    invalid_connection.desc = -1;
    clients.push_lock(invalid_connection);
    Room invalid_room = {};
    invalid_room.player_a = -1;
    rooms.push_lock(invalid_room);

    int server_socket_descriptor;
    int connection_socket_descriptor;
    int bind_result;
    int listen_result;
    char reuse_addr_val = 1;
    sockaddr_in server_address;

    //inicjalizacja gniazda serwera

    memset(&server_address, 0, sizeof(sockaddr));
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = htonl(INADDR_ANY);
    server_address.sin_port = htons(SERVER_PORT);

    server_socket_descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_descriptor < 0) {
        fprintf(stderr, "%s: Błąd przy próbie utworzenia gniazda..\n", argv[0]);
        exit(1);
    }
    setsockopt(server_socket_descriptor, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse_addr_val, sizeof(reuse_addr_val));

    bind_result = bind(server_socket_descriptor, (sockaddr*)&server_address, sizeof(sockaddr));
    if (bind_result < 0) {
        fprintf(stderr, "%s: Błąd przy próbie dowiązania adresu IP i numeru portu do gniazda.\n", argv[0]);
        exit(1);
    }

    listen_result = listen(server_socket_descriptor, QUEUE_SIZE);
    if (listen_result < 0) {
        fprintf(stderr, "%s: Błąd przy próbie ustawienia wielkości kolejki.\n", argv[0]);
        exit(1);
    }

    while(1) {
        connection_socket_descriptor = accept(server_socket_descriptor, NULL, NULL);
        if (connection_socket_descriptor < 0) {
            fprintf(stderr, "%s: Błąd przy próbie utworzenia gniazda dla połączenia.\n", argv[0]);
            exit(1);
        }

        handle_connection(connection_socket_descriptor);
    }

    close(server_socket_descriptor);
    return(0);
}
