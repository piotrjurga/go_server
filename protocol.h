#pragma once

#include <stdint.h>
#include <unistd.h>
#include "game_logic.h"

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

void send_request(int connection, Request r);
void send_request_async(int connection, Request r);

enum ResponseType {
    RESPONSE_NONE,
    RESPONSE_NEW_MOVE,
    RESPONSE_NEW_ROOM_RESULT,
    RESPONSE_JOIN_RESULT,
    RESPONSE_PLAYER_JOINED,
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

struct Response {
    ResponseType type;
    union {
        ResponseNewMove new_move;
        ResponseNewRoomResult new_room_result;
        ResponseJoinResult join_result;
    };
};
