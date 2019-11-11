#include "game_logic.h"
#include <cassert>

inline Stone other_stone_color(Stone s) {
    assert(s & 0b01);
    return (Stone)(s ^ 0b10);
}

inline Stone bool_to_stone(bool b) {
    return (Stone)((b << 1) | 0b1);
}

Stone Board::stone(int i, int j) {
    assert(i >= 0 && i < size);
    assert(j >= 0 && j < size);

    uint32_t stone_bit = stones[i*size + j] != 0;
    uint32_t color_bit = colors[i*size + j] != 0;
    uint32_t result = stone_bit | (color_bit << 1);
    return (Stone)result;
}

void Board::set(int i, int j, Stone s) {
    assert(i >= 0 && i < size);
    assert(j >= 0 && j < size);

    if(s == STONE_NONE) {
        stones.reset(i*size + j);
        colors.reset(i*size + j);
        return;
    }
    stones.set(i*size + j);
    if(s == STONE_WHITE)
        colors.set(i*size + j);
}

std::vector<v2> Board::get_group(int x, int y) {
    std::vector<v2> group;
    std::queue<v2> queue;

    if(x < 0 || x >= size || y < 0 || y >= size)
        return group;

    bool visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

    auto group_type = stone(x,y);
    if(group_type == STONE_NONE)
        return group;

    queue.push({x,y});
    while(!queue.empty()) {
        auto el = queue.front();
        queue.pop();

        auto s = stone(el.x, el.y);
        if(s != group_type) continue;

        group.push_back(el);

        int dx[] = {1, 0, -1, 0};
        int dy[] = {0, 1, 0, -1};
        for(int i = 0; i < 4; i++) {
            int nx = el.x + dx[i];
            int ny = el.y + dy[i];
            if(nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
            if(!visited[nx][ny]) {
                visited[nx][ny] = true;
                queue.push({nx, ny});
            }
        }
    }

    return group;
}

int Board::count_liberties(std::vector<v2> group) {
    assert(group.size() > 0);
    int result = 0;
    bool visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

    v2 delta[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};

    for(auto it : group) {
        for(int i = 0; i < 4; i++) {
            int x = it.x + delta[i].x;
            int y = it.y + delta[i].y;
            if(x >= 0 && x < size && y >= 0 && y < size) {
                if(visited[x][y]) continue;

                result += (stone(x, y) == STONE_NONE);
                visited[x][y] = true;
            }
        }
    }

    return result;
}

void MoveLog::register_move(int i, int j) {
    assert(i >= -2 && i < MAX_BOARD_SIZE);
    assert(j >= 0 && j < MAX_BOARD_SIZE);
    assert(move_count < 512);

    v2_8 v = {(int8_t)i, (int8_t)j};
    moves[move_count++] = v;
    if(last_valid_move_count < move_count)
        last_valid_move_count = move_count;
}

void MoveLog::register_remove(std::vector<v2> stones) {
    removed_count[move_count-1] = stones.size();
    for(auto it : stones) {
        removed[removed_count_total++] = {(int8_t)it.x, (int8_t)it.y};
    }
}

inline bool GameData::active_player() {
    return (bool)(log.move_count & 0x1);
}

// returns true if move was successful, otherwise returns false
// and leaves the GameData unchanged
bool GameData::maybe_make_move(int i, int j) {
    Stone s = active_player() ? STONE_WHITE : STONE_BLACK;

    if(i == -1 || i == -2) {
        // pass or resign
        log.register_move(i, j);
        return true;
    } else if(i < 0 || i >= board.size || j < 0 || j >= board.size) {
        return false;
    }

    if(board.stone(i, j) != STONE_NONE)
        return false;

    GameData previous_state = *this;
    previous_state.undo_move();

    board.set(i, j, s);

    bool visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};
    std::vector<v2> stones_to_remove;

    v2 delta[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
    for(int it = 0; it < 4; it++) {
        int x = i + delta[it].x;
        int y = j + delta[it].y;
        if(x < 0 || x >= board.size ||
           y < 0 || y >= board.size)
            continue;

        if(board.stone(x, y) == s ||
           board.stone(x, y) == STONE_NONE)
            continue;

        auto g = board.get_group(x, y);
        int libs = board.count_liberties(g);
        if(libs == 0) {
            for(v2 it : g) {
                if(!visited[it.x][it.y]) {
                    stones_to_remove.push_back(it);
                    visited[it.x][it.y] = true;
                }
            }
        }
    }

    auto current_move_group = board.get_group(i, j);
    int libs = board.count_liberties(current_move_group);
    if(libs == 0 && stones_to_remove.size() == 0) {
        board.set(i, j, STONE_NONE);
        return false;
    }

    for(v2 it : stones_to_remove)
        board.set(it.x, it.y, STONE_NONE);
    
    // verify against ko rule
    if(previous_state.board.stones == board.stones) {
        board.set(i, j, STONE_NONE);
        for(v2 it : stones_to_remove)
            board.set(it.x, it.y, other_stone_color(s));
        return false;
    }

    // move successful
    log.register_move(i, j);
    log.register_remove(stones_to_remove);
    return true;
}

bool GameData::pass() {
    return maybe_make_move(-1, 0);
}

bool GameData::resign() {
    return maybe_make_move(-2, 0);
}

void Board::count_region(int i, int j, bool visited[19][19],
                         float *black_points, float *white_points) {
    float black_score = 0;
    float white_score = 0;
    if(!black_points) black_points = &black_score;
    if(!white_points) white_points = &white_score;
    if(i < 0 || i >= size || j < 0 || j >= size) return;
    Stone wall_type = STONE_NONE;
    int region_size = 0;
    bool assign_points = true;
    std::queue<v2> queue;
    queue.push({i,j});
    visited[i][j] = true;

    while(!queue.empty()) {
        auto el = queue.front();
        queue.pop();

        auto s = stone(el.x, el.y);
        if(s != STONE_NONE) {
            if(wall_type == STONE_NONE)
                wall_type = s;
            else if(s != wall_type)
                assign_points = false;
            continue;
        }
        region_size++;

        int dx[] = {1, 0, -1, 0};
        int dy[] = {0, 1, 0, -1};
        for(int i = 0; i < 4; i++) {
            int nx = el.x + dx[i];
            int ny = el.y + dy[i];
            if(nx < 0 || nx >= size || ny < 0 || ny >= size)
                continue;
            if(!visited[nx][ny]) {
                visited[nx][ny] = true;
                queue.push({nx, ny});
            }
        }
    }

    if(assign_points) {
        if(wall_type == STONE_WHITE)
            *white_points += region_size;
        if(wall_type == STONE_BLACK)
            *black_points += region_size;
    }
}

Stone GameData::winner(float *black_points, float *white_points) {
    if(log.move_count == 0) return STONE_NONE;
    auto last_move = log.moves[log.move_count-1];
    if(last_move.x == -2)
        return bool_to_stone(active_player());
    if(last_move.x != -1)   return STONE_NONE;
    if(log.move_count == 1) return STONE_NONE;

    auto previous_move = log.moves[log.move_count-2];
    if(previous_move.x != -1) return STONE_NONE;

    // two passes in a row, we need to count the score

    bool visited[19][19] = {};
    float white_score = 0;
    float black_score = 0;
    if(!white_points) white_points = &white_score;
    if(!black_points) black_points = &black_score;
    for(int i = 0; i < board.size; i++) {
        for(int j = 0; j < board.size; j++) {
            if(!visited[i][j])
                board.count_region(i, j, visited, black_points, white_points);
        }
    }

    for(int i = 0; i < log.move_count; i++) {
        if(i % 2)
            *white_points += log.removed_count[i];
        else
            *black_points += log.removed_count[i];
    }

    float komi = 3.5f;
    if(board.size > 12) komi = 6.5f;
    *white_points += komi;

    if(*white_points > *black_points) return STONE_WHITE;
    else return STONE_BLACK;
}

void GameData::redo_move() {
    if(log.last_valid_move_count == log.move_count)
        return;
    v2_8 _v = log.moves[log.move_count++];
    v2 v = {(int32_t)_v.x, (int32_t)_v.y};
    maybe_make_move(v.x, v.y);
}

void GameData::undo_move() {
    if(log.move_count == 0) return;
    Stone s = active_player() ? STONE_WHITE : STONE_BLACK;
    log.move_count--;

    v2_8 _v = log.moves[log.move_count];
    v2 v = {(int32_t)_v.x, (int32_t)_v.y};
    if(v.x == -1 || v.x == -2) return;
    board.set(v.x, v.y, STONE_NONE);

    // place back all removed stones
    while(log.removed_count[log.move_count] > 0) {
        v2_8 _v = log.removed[--log.removed_count_total];
        v2 v = {(int32_t)_v.x, (int32_t)_v.y};
        board.set(v.x, v.y, s);
        log.removed_count[log.move_count]--;
    }
    
}

void GameData::undo_move(int n) {
    for(int i = 0; i < n; i++) {
        undo_move();
    }
}
