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
#include <vector>

#include "game_logic.h"
#include "protocol.h"

#define SERVER_PORT 1234
#define QUEUE_SIZE 5

pthread_mutex_t games_mutex;
static std::vector<GameData> games;
static std::vector<v2> players;

struct ThreadData {
    int connection;
};

void *handle_client(void *t_data) {
    pthread_detach(pthread_self());
    ThreadData *th_data = (ThreadData *)t_data;
    int connection = th_data->connection;

    int32_t active_game_id = -1;

    bool done = false;
    while(!done) {
        Request r = REQUEST_NONE;
        read(connection, &r, sizeof(Request));
        switch(r) {
            case REQUEST_NEW_GAME: {
                printf("requested new game by connection %d\n", connection);
                int32_t board_size;
                read(connection, &board_size, sizeof(int32_t));

                int32_t new_game_id = -1;
                if(active_game_id != -1) {
                    write(connection, &new_game_id, sizeof(int32_t));
                    break;
                }

                GameData new_game = {};
                new_game.board.size = board_size;

                pthread_mutex_lock(&games_mutex);
                new_game_id = games.size();
                games.push_back(new_game);
                players.push_back({connection, 0});
                pthread_mutex_unlock(&games_mutex);

                write(connection, &new_game_id, sizeof(int32_t));
                printf("new game id: %d\n", new_game_id);
            } break;

            case REQUEST_JOIN_GAME: {
                int32_t game_id = -1;
                read(connection, &game_id, sizeof(int32_t));
                printf("reqested join id %d by connection %d\n", game_id, connection);

                int32_t success = 0;
                if(active_game_id != -1) {
                    write(connection, &success, sizeof(int32_t));
                    break;
                }

                pthread_mutex_lock(&games_mutex);
                if(game_id < 0 || game_id >= (int32_t)games.size() || players[game_id].y != 0) {
                    write(connection, &success, sizeof(int32_t));
                    pthread_mutex_unlock(&games_mutex);
                    break;
                }

                players[game_id].y = connection;
                active_game_id = game_id;
                pthread_mutex_unlock(&games_mutex);
                success = 1;
                write(connection, &success, sizeof(int32_t));
                puts("join success");
             } break;

            case REQUEST_NONE:
            case REQUEST_EXIT:
                puts("req exit"); //DEBUG
                done = true;
                break;
        }
    }

    free(th_data);
    pthread_exit(NULL);
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
    games_mutex = PTHREAD_MUTEX_INITIALIZER;

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
    pthread_mutex_destroy(&games_mutex);
    return(0);
}
