#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include "game_logic.h"
#include "protocol.h"

#include <SDL.h>
#include <GL/gl3w.h>

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int connect_to_server(const char *server_name, uint16_t port_number) {
   int connection_socket_descriptor;
   int connect_result;
   sockaddr_in server_address;
   hostent *server_host_entity;

   server_host_entity = gethostbyname(server_name);
   if(!server_host_entity) {
      fprintf(stderr, "Nie można uzyskać adresu IP serwera.\n");
      return 0;
   }

   connection_socket_descriptor = socket(PF_INET, SOCK_STREAM, 0);
   if(connection_socket_descriptor < 0) {
      fprintf(stderr, "Błąd przy probie utworzenia gniazda.\n");
      return 0;
   }

   memset(&server_address, 0, sizeof(struct sockaddr));
   server_address.sin_family = AF_INET;
   memcpy(&server_address.sin_addr.s_addr, server_host_entity->h_addr, server_host_entity->h_length);
   server_address.sin_port = htons(port_number);

   connect_result = connect(connection_socket_descriptor, (sockaddr *)&server_address, sizeof(struct sockaddr));
   if (connect_result < 0)
   {
       fprintf(stderr, "Błąd przy próbie połączenia z serwerem (%s:%i).\n", server_name, port_number);
       return 0;
   }

   return connection_socket_descriptor;
}

static bool ready_to_make_move = false;

struct ClientState {
    int connection;
    bool got_opponent_move;
    v2 opponent_move;
    bool got_room_id;
    int32_t room_id;
    bool got_join_result;
    bool join_result;
    bool player_joined;
};

void *client_thread(void *t_data) {
    pthread_detach(pthread_self());
    ClientState *cs = (ClientState *)t_data;

    bool done = false;
    while(!done) {
        Response r = {};
        read(cs->connection, &r, sizeof(Response));

        switch(r.type) {
            case RESPONSE_NEW_MOVE: {
                v2_8 m = r.new_move.move;
                cs->opponent_move = {(int)m.x, (int)m.y};
                cs->got_opponent_move = true;
            } break;
            case RESPONSE_NEW_ROOM_RESULT: {
                assert(cs->room_id == 0);
                cs->room_id = r.new_room_result.room_id;
                cs->got_room_id = true;
            } break;
            case RESPONSE_JOIN_RESULT: {
                cs->join_result = r.join_result.success;
                cs->got_join_result = true;
            } break;
            case RESPONSE_PLAYER_JOINED: {
                cs->player_joined = true;
            } break;
            case RESPONSE_NONE: {
                puts("got response none!");
                done = true;
            } break;
        }
    }

    pthread_exit(0);
}

struct ReadThreadData {
    int connection;
    void *buffer;
    int size;
    bool *finished;
};

void *read_thread(void *t_data) {
    pthread_detach(pthread_self());
    ReadThreadData *th_data = (ReadThreadData *)t_data;
    int connection = th_data->connection;
    void *buffer = th_data->buffer;
    int size = th_data->size;
    bool *finished = th_data->finished;

    read(connection, buffer, size);
    *finished = true;

    free(th_data);
    pthread_exit(0);
}

void read_async(int connection, void *buffer, int size, bool *finished) {
    pthread_t thread;
    ReadThreadData *t_data = (ReadThreadData *)malloc(sizeof(ReadThreadData));
    t_data->connection = connection;
    t_data->buffer     = buffer;
    t_data->size       = size;
    t_data->finished   = finished;
    pthread_create(&thread, 0, read_thread, (void *)t_data);
}

inline bool is_inside(ImVec2 p, ImVec4 rect) {
    return p.x <= rect.z && p.x >= rect.x &&
           p.y <= rect.w && p.y >= rect.y;
}

void send_last_move(int connection, GameData *gd) {
    Request r = {};
    r.type = REQUEST_MAKE_MOVE;
    int move_count = gd->log.move_count;
    r.make_move.move = gd->log.moves[move_count-1];
    send_request_async(connection, r);
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

// returns true if move was made
bool draw_board_interact(ImDrawList *dl, GameData *gd, ImVec2 p, float dim) {
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
                    bool res = gd->maybe_make_move(i, j);
                    return res;
                }
                return false;
            }
        }
    }
    return false;
}

void draw_board_interactive_local(ImDrawList *dl, GameData *gd, ImVec2 p, float dim) {
    if(ImGui::IsMouseReleased(1)) gd->undo_move();

    draw_board(dl, &gd->board, p, dim);
    draw_board_interact(dl, gd, p, dim);
}


void draw_board_interactive_online(ClientState *cs, ImDrawList *dl, GameData *gd, ImVec2 p, float dim) {
    draw_board(dl, &gd->board, p, dim);

    if(ready_to_make_move) {
        bool made_move = draw_board_interact(dl, gd, p, dim);
        if(made_move) {
            ready_to_make_move = false;
            send_last_move(cs->connection, gd);
        }
    } else {
        if(cs->got_opponent_move) {
            cs->got_opponent_move = false;
            bool res = gd->maybe_make_move((int)cs->opponent_move.x,
                                           (int)cs->opponent_move.y);
            // TODO(piotr): maybe handle this error by downloading
            // the whole board state from the server
            assert(res && "server sent an illegal opponent move");
            ready_to_make_move = true;
        }
    }
}

int main(int, char**) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
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

    if (err) {
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
    ClientState cs = {};

    // Main loop
    bool done = false;
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
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
        static bool the_game_is_on = false;
        if(the_game_is_on) {
            float dim = 500.f;
            //ImGui::SetNextWindowPos(ImVec2(20, 20));
            //ImGui::SetNextWindowSize(ImVec2(dim, dim+20.f));

            ImGui::Begin("Game");

            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.x -= 10.f;
            p.y -= 10.f;

            static GameData gd = {};
            if(!gd.board.size) {
                gd.board.size = 9;
            }
            draw_board_interactive_online(&cs, draw_list, &gd, p, dim);

            ImGui::End();
        }

        // server interaction debug window
        {
            ImGui::Begin("Server");
            ImGui::Text("connection: %x", cs.connection);
            if(ImGui::Button("Connect")) {
                cs.connection = connect_to_server("127.0.0.1", 1234);
                pthread_t thread;
                pthread_create(&thread, 0, client_thread, (void *)&cs);
            }

            if(ImGui::Button("Request new room")) {
                Request r = {};
                r.type = REQUEST_NEW_ROOM;
                r.new_room.board_size = 9;
                send_request_async(cs.connection, r);
            }
            if(cs.got_room_id && cs.room_id != -1) {
                cs.got_room_id = false;
                puts("got room id");
                ready_to_make_move = true;
            }
            if(cs.player_joined) {
                cs.player_joined = false;
                puts("player joined");
                the_game_is_on = true;
            }
            ImGui::Text("Game id: %d", cs.room_id);

            if(ImGui::Button("Request join room")) {
                Request r = {};
                r.type = REQUEST_JOIN_ROOM;
                r.join_room.room_id = 0;
                send_request_async(cs.connection, r);
            }
            if(cs.got_join_result && cs.join_result) {
                cs.got_join_result = false;
                the_game_is_on = true;
            }
            ImGui::Text("Join success: %d", cs.join_result);
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
