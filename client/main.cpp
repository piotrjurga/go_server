#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <SDL.h>
#include <bitset>
#include <queue>
#include <unordered_map>
#include <vector>

#include <GL/gl3w.h>

#include <stdint.h>

#define MAX_BOARD_SIZE 19

struct v2 {
    union {
        struct {
            int32_t x;
            int32_t y;
        };
        int64_t all;
    };
};

struct v2_8 { int8_t x; int8_t y; };

enum Stone {
    STONE_NONE  = 0b00,
    STONE_BLACK = 0b01,
    STONE_WHITE = 0b11,
};

inline Stone other_stone_color(Stone s) {
    assert(s & 0b01);
    return (Stone)(s ^ 0b10);
}

struct Board {
    std::bitset<384> stones;
    std::bitset<384> colors;
    int32_t size;

    Stone stone(int i, int j);
    void set(int i, int j, Stone s);
    std::vector<v2> get_group(int x, int y);
    int count_liberties(std::vector<v2> group);
};

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

    bool visited[MAX_BOARD_SIZE][MAX_BOARD_SIZE] = {};

    auto group_type = stone(x,y);
    if(group_type == STONE_NONE)
        return group;

    queue.push({x,y});
    while(!queue.empty()) {
        auto el = queue.front();
        queue.pop();

        if(el.x < 0 || el.x >= size ||
           el.y < 0 || el.y >= size)
            continue;

        visited[el.x][el.y] = true;
        auto s = stone(el.x, el.y);
        if(s != group_type) continue;

        group.push_back(el);

        if(!visited[el.x-1][el.y]) queue.push({el.x-1, el.y});
        if(!visited[el.x+1][el.y]) queue.push({el.x+1, el.y});
        if(!visited[el.x][el.y-1]) queue.push({el.x, el.y-1});
        if(!visited[el.x][el.y+1]) queue.push({el.x, el.y+1});
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

void MoveLog::register_move(int i, int j) {
    assert(i >= 0 && i < MAX_BOARD_SIZE);
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

struct GameData {
    Board board;
    MoveLog log;

    bool active_player();
    bool maybe_make_move(int i, int j);
    void undo_move();
    void undo_move(int n);
    void redo_move();
};

inline bool GameData::active_player() {
    return (bool)(log.move_count & 0x1);
}

// returns true if move was successful, otherwise returns false
// and leaves the GameData unchanged
bool GameData::maybe_make_move(int i, int j) {
    Stone s = active_player() ? STONE_WHITE : STONE_BLACK;
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

inline bool is_inside(ImVec2 p, ImVec4 rect) {
    return p.x <= rect.z && p.x >= rect.x &&
           p.y <= rect.w && p.y >= rect.y;
}

void draw_board(ImDrawList *dl, Board *board, ImVec2 p, float dim) {
    // draw goban
    ImU32 line_color = 0xffffffff;
    int size = board->size;
    int fields = size + 1;
    float field_sz = dim / fields;
    float thickness = 2.f;
    for(int i = 1; i < fields; i++) {
        dl->AddLine(ImVec2(p.x + i*field_sz, p.y + field_sz),
                    ImVec2(p.x + i*field_sz, p.y + dim-field_sz),
                    line_color, thickness);
        dl->AddLine(ImVec2(p.x + field_sz,     p.y + i*field_sz),
                    ImVec2(p.x + dim-field_sz, p.y + i*field_sz),
                    line_color, thickness);
    }

    // draw stones
    for(int i = 0; i < size; i++) {
        for(int j = 0; j < size; j++) {
            Stone s = board->stone(i, j);
            if(s != STONE_NONE) {
                ImU32 color = 0xffffffff;
                if(s == STONE_BLACK)
                    color = 0xff444444;
                ImVec2 center = ImVec2(p.x + (i+1)*field_sz, p.y + (j+1)*field_sz);
                dl->AddCircleFilled(center, field_sz*0.5f, color, 32);
            }
        }
    }
}

void draw_board_interactive_local(ImDrawList *dl, GameData *gd, ImVec2 p, float dim) {
    if(ImGui::IsMouseReleased(1)) gd->undo_move();

    draw_board(dl, &gd->board, p, dim);

    // draw hover stone

    int size = gd->board.size;
    float field_sz = dim / (size + 1);
    ImGuiIO &io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    for(int i = 0; i < size; i++) {
        for(int j = 0; j < size; j++) {
            if(gd->board.stone(i, j) != STONE_NONE) continue;
            ImVec2 center = ImVec2(p.x + (i+1)*field_sz, p.y + (j+1)*field_sz);
            ImVec4 field = ImVec4(center.x - 0.5f * field_sz,
                                  center.y - 0.5f * field_sz,
                                  center.x + 0.5f * field_sz,
                                  center.y + 0.5f * field_sz);

            if(is_inside(mouse, field)) {

                ImU32 color = 0x44444444;
                if(gd->active_player())
                    color = 0x44ffffff;
                dl->AddCircleFilled(center, field_sz*0.5f, color, 32);
                if(ImGui::IsMouseReleased(0)) {
                    gd->maybe_make_move(i, j);
                }

                return;
            }
        }
    }
}

int main(int, char**)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
    {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    // Decide GL+GLSL versions
#if __APPLE__
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG); // Always required on Mac
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("Go Client", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    bool err = gl3wInit() != 0;

    if (err)
    {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsClassic();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer bindings
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Read 'misc/fonts/README.txt' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != NULL);

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // Main loop
    bool done = false;
    while (!done)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT)
                done = true;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                done = true;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

#if 0
        bool show_demo_window = true;
        ImGui::ShowDemoWindow(&show_demo_window);
#endif
        // game window
        {
            float dim = 500.f;
            ImGui::SetNextWindowPos(ImVec2(20, 20));
            ImGui::SetNextWindowSize(ImVec2(dim, dim+20.f));
            ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove |
                                  ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoCollapse;

            ImGui::Begin("Game", 0, wf);

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.x -= 10.f;
            p.y -= 10.f;

            static GameData gd = {};
            if(!gd.board.size) {
                gd.board.size = 9;
            }
            draw_board_interactive_local(draw_list, &gd, p, dim);

            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
