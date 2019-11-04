#pragma once

#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include "game_logic.h"

struct Connection {
    int desc;
    pthread_mutex_t mutex;
};

enum RequestType {
    REQUEST_NONE,
    REQUEST_NEW_ROOM,
    REQUEST_JOIN_ROOM,
    REQUEST_MAKE_MOVE,
    REQUEST_LIST_ROOMS,
    REQUEST_EXIT
};

struct RequestNewRoom {
    int32_t board_size;
    char name[16];
};

struct RequestJoinRoom {
    int32_t room_id;
};

struct RequestMakeMove {
    v2_8 move;
};


struct Request {
    RequestType type;
    union {
        RequestNewRoom  new_room;
        RequestJoinRoom join_room;
        RequestMakeMove make_move;
    };
};

enum ResponseType {
    RESPONSE_NONE,
    RESPONSE_NEW_MOVE,
    RESPONSE_NEW_ROOM_RESULT,
    RESPONSE_JOIN_RESULT,
    RESPONSE_PLAYER_JOINED,
    RESPONSE_LIST_ROOMS,
};

struct ResponseNewMove {
    int32_t room_id;
    v2_8 move;
};

struct ResponseNewRoomResult {
    int32_t room_id;
};

struct ResponseJoinResult {
    bool success;
};

struct ResponseListRooms {
    int32_t size;
};

struct Response {
    ResponseType type;
    union {
        ResponseNewMove new_move;
        ResponseNewRoomResult new_room_result;
        ResponseJoinResult join_result;
        ResponseListRooms list_rooms;
    };
};

#define read_struct(connection, buf) read_size(connection, (void *)buf, sizeof(*buf))
int read_size(int connection, void *data, size_t size);

#define write_struct(connection, data) write_size(connection, (void *)data, sizeof(*data))
int write_size(int connection, void *data, size_t size);
int write_size(Connection *connection, void *data, size_t size);
