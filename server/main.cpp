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

#include "game_logic.h"
#include "protocol.h"

#define SERVER_PORT 1234
#define QUEUE_SIZE 5

struct Room {
    GameData game;
    int32_t player_a;
    int32_t player_b;
    char name[16];
};

int first_empty_slot(std::vector<Room> &vec) {
    int i = 0;
    for(; i < (int)vec.size(); i++) {
        if(vec[i].player_a == 0)
            return i;
    }
    vec.resize(vec.size()+1);
    return i;
}

// TODO(piotr): move to a data structure for rooms better for multithreading?
pthread_mutex_t rooms_mutex;
static std::vector<Room> rooms;

struct ThreadData {
    int connection;
};

void *handle_client(void *t_data) {
    pthread_detach(pthread_self());
    ThreadData *th_data = (ThreadData *)t_data;
    int connection = th_data->connection;

    int32_t active_room_id = -1;

    bool done = false;
    while(!done) {
        Request r = {};
        Response res = {};
        read(connection, &r, sizeof(Request));
        //printf("received request of type %d\n", r.type);
        switch(r.type) {
            case REQUEST_NEW_ROOM: {
                printf("requested new room by connection %d\n", connection);
                int32_t new_room_id = -1;
                int board_size = r.new_room.board_size;
                res.type = RESPONSE_NEW_ROOM_RESULT;
                if(active_room_id != -1 || board_size < 2 || board_size > 19) {
                    res.new_room_result.room_id = -1;
                    write(connection, &res, sizeof(Response));
                    break;
                }

                Room new_room = {};
                new_room.game.board.size = board_size;
                new_room.player_a = connection;

                pthread_mutex_lock(&rooms_mutex);
                new_room_id = first_empty_slot(rooms);
                active_room_id = new_room_id;
                rooms[new_room_id] = new_room;
                pthread_mutex_unlock(&rooms_mutex);

                res.new_room_result.room_id = new_room_id;
                write(connection, &res, sizeof(Response));
                printf("new room id: %d\n", new_room_id);
            } break;

            case REQUEST_JOIN_ROOM: {
                res.type = RESPONSE_JOIN_RESULT;
                int32_t room_id = r.join_room.room_id;
                printf("reqested join id %d by connection %d\n", room_id, connection);

                res.join_result.success = false;
                if(active_room_id != -1) {
                    write(connection, &res, sizeof(Response));
                    break;
                }

                pthread_mutex_lock(&rooms_mutex);
                if(room_id < 0 || room_id >= (int32_t)rooms.size() || rooms[room_id].player_b != 0) {
                    write(connection, &res, sizeof(Response));
                    pthread_mutex_unlock(&rooms_mutex);
                    break;
                }

                rooms[room_id].player_b = connection;
                int other_player = rooms[room_id].player_a;
                active_room_id = room_id;
                pthread_mutex_unlock(&rooms_mutex);

                res.join_result.success = true;
                write(connection, &res, sizeof(Response));

                Response res2 = {};
                res2.type = RESPONSE_PLAYER_JOINED;
                // TODO(piotr): put a mutex on every connection?
                write(other_player, &res2, sizeof(Response));
                puts("join success");
             } break;

            case REQUEST_MAKE_MOVE: {
                v2_8 move = r.make_move.move;
                int x = (int)move.x, y = (int)move.y;
                printf("reqested make move (%d, %d) by connection %d\n", x, y, connection);
                pthread_mutex_lock(&rooms_mutex);
                bool result = rooms[active_room_id].game.maybe_make_move(x, y);
                // TODO(piotr): handle this error
                assert(result && "illegal move in REQUEST_MAKE_MOVE");

                int other_player = rooms[active_room_id].player_a;
                if(other_player == connection)
                    other_player = rooms[active_room_id].player_b;
                // TODO(piotr): broadcast this message to everyone
                // watching the game
                printf("sending move to player %d\n", other_player);
                res.type = RESPONSE_NEW_MOVE;
                res.new_move.room_id = active_room_id;
                res.new_move.move.x = x;
                res.new_move.move.y = y;
                write(other_player, &res, sizeof(Response));

                pthread_mutex_unlock(&rooms_mutex);
            } break;

            case REQUEST_LIST_ROOMS: {
                pthread_mutex_lock(&rooms_mutex);
/*
                int size = rooms.size();
                write(connection, &size, sizeof(int));
                for(int i = 0; i < size; i++) {
                    write(connection, &rooms[i].game.board, sizeof(board));
                }
*/
                pthread_mutex_unlock(&rooms_mutex);
            } break;

            case REQUEST_NONE: {
                printf("got request none from %d", connection);
                done = true;
            } break;
            case REQUEST_EXIT: {
                printf("got reqest exit from %d", connection);
                done = true;
            } break;

            default: {
                printf("received unrecognized request type %d\n", (int)r.type);
            }
        }
    }

    free(th_data);
    pthread_exit(0);
}

void handle_connection(int connection_socket_descriptor) {
    int create_result = 0;

    pthread_t thread1;

    ThreadData *t_data = (ThreadData *)malloc(sizeof(ThreadData));
    t_data->connection = connection_socket_descriptor;

    create_result = pthread_create(&thread1, NULL, handle_client, (void *)t_data);
    if (create_result) {
        printf("Błąd przy próbie utworzenia wątku, kod błędu: %d\n", create_result);
        exit(-1);
    }
}

int main(int argc, char **argv) {
    rooms_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_destroy(&rooms_mutex);
    return(0);
}
