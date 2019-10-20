#pragma once

#include <bitset>
#include <queue>
#include <vector>
#include <stdint.h>

#define MAX_BOARD_SIZE 19

struct v2 { int32_t x; int32_t y; };
struct v2_8 { int8_t x; int8_t y; };

enum Stone {
    STONE_NONE  = 0b00,
    STONE_BLACK = 0b01,
    STONE_WHITE = 0b11,
};

inline Stone other_stone_color(Stone s);

struct Board {
    std::bitset<384> stones;
    std::bitset<384> colors;
    int32_t size;

    Stone stone(int i, int j);
    void set(int i, int j, Stone s);
    std::vector<v2> get_group(int x, int y);
    int count_liberties(std::vector<v2> group);
};

struct MoveLog {
    int16_t move_count;
    int16_t last_valid_move_count;
    int16_t removed_count_total;

    v2_8    moves[512];

    // for each move, how many stones were removed
    int16_t removed_count[512];

    // list of all removed stones
    v2_8    removed[512];

    void register_move(int i, int j);
    void register_remove(std::vector<v2> stones);
};

struct GameData {
    Board board;
    MoveLog log;

    bool active_player();
    bool maybe_make_move(int i, int j);
    void undo_move();
    void undo_move(int n);
    void redo_move();
};

