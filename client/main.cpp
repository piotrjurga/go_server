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
#include <errno.h>

#define print_bytes(p) print_bytes_size(p, sizeof(*p))
void print_bytes_size(void *p, size_t size) {
    for(uint32_t i = 0; i < size; i++) {
        printf("%02x ", (int)(int8_t)*((int8_t *)p + i));
    }
    puts("");
}

struct ThreadSendRequestData {
    int connection;
    pthread_mutex_t *connection_mutex;
    Request request;
};

void *thread_send_request(void *t_data) {
    pthread_detach(pthread_self());
    ThreadSendRequestData *th_data = (ThreadSendRequestData *)t_data;
    pthread_mutex_lock(th_data->connection_mutex);
    write_struct(th_data->connection, &th_data->request);
    pthread_mutex_unlock(th_data->connection_mutex);
    free(th_data);
    pthread_exit(0);
}

void send_request_async(int connection, pthread_mutex_t *con_mutex, Request r) {
    pthread_t thread;
    ThreadSendRequestData *thread_data = (ThreadSendRequestData *)malloc(sizeof(ThreadSendRequestData));
    thread_data->connection = connection;
    thread_data->connection_mutex = con_mutex;
    thread_data->request = r;
    pthread_create(&thread, 0, thread_send_request, (void *)thread_data);
}

void send_request_async(Connection *con, Request r) {
    send_request_async(con->desc, &con->mutex, r);
}

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

struct ClientState {
    Connection connection;

    bool ready_to_make_move;
    bool got_opponent_move;
    v2 opponent_move;
    bool got_room_id;
    int32_t room_id;
    bool got_join_result;
    bool join_result;
    bool player_joined;
    bool got_game_list;
    bool other_player_left;
    bool connection_lost;
    bool update_game_data;
    GameData game_data; //the data sent by server to update local game data

    std::vector<int> room_ids;
    std::vector<std::string> names;
    std::vector<bool> can_join;
    std::vector<Board> games;
};

void *client_thread(void *t_data) {
    pthread_detach(pthread_self());
    ClientState *cs = (ClientState *)t_data;

    bool done = false;
    while(!done) {
        Response r = {};
        int err = read_struct(cs->connection.desc, &r);
        if(err) {
            printf("error reading server response: %s\n", strerror(errno));
            cs->connection_lost = true;
            pthread_exit(0);
        }

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
            case RESPONSE_LIST_ROOMS: {
                int size = r.list_rooms.size;
                cs->names.clear();
                cs->can_join.resize(size);
                cs->games.resize(size);
                cs->room_ids.resize(size);
                printf("reading %d rooms...\n", size);
                for(int i = 0; i < size; i++) {
                    int id = 0;
                    read_struct(cs->connection.desc, &id);
                    cs->room_ids[i] = id;
                    char name[16] = {};
                    read_size(cs->connection.desc, name, 16);
                    cs->names.push_back(name);
                    bool can_join = false;
                    read_struct(cs->connection.desc, &can_join);
                    cs->can_join[i] = can_join;
                    read_struct(cs->connection.desc, &cs->games[i]);
                    printf("room %d:\n\tname: %s\n\tcan_join: %d\n", i, name, (int)can_join);
                }
                cs->got_game_list = true;
            } break;
            case RESPONSE_ILLEGAL_MOVE: {
                read_struct(cs->connection.desc, &cs->game_data);
                cs->update_game_data = true;
            } break;
            case RESPONSE_NONE: {
                puts("got response none!");
            } break;
            case RESPONSE_EXIT: {
                cs->other_player_left = true;
                Request req = {};
                req.type = REQUEST_LEAVE_ROOM;
                write_struct(&cs->connection, &req);
            } break;
        }
    }

    pthread_exit(0);
}

inline bool is_inside(ImVec2 p, ImVec4 rect) {
    return p.x <= rect.z && p.x >= rect.x &&
           p.y <= rect.w && p.y >= rect.y;
}

void send_last_move(Connection *con, GameData *gd) {
    puts("sending last move");
    Request r = {};
    r.type = REQUEST_MAKE_MOVE;
    int move_count = gd->log.move_count;
    r.make_move.move = gd->log.moves[move_count-1];
    send_request_async(con, r);
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

    if(cs->ready_to_make_move) {
        bool made_move = draw_board_interact(dl, gd, p, dim);
        if(made_move) {
            cs->ready_to_make_move = false;
            send_last_move(&cs->connection, gd);
        }
    } else {
        if(cs->got_opponent_move) {
            cs->got_opponent_move = false;
            bool res = gd->maybe_make_move((int)cs->opponent_move.x,
                                           (int)cs->opponent_move.y);
            // TODO(piotr): maybe handle this error by downloading
            // the whole board state from the server
            if(!res) puts("server sent an illegal opponent move");
            cs->ready_to_make_move = true;
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

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    ClientState cs = {};
    GameData    gd = {};

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
            ImGui::Begin("Game");
            bool pass = ImGui::Button("Pass");
            ImGui::SameLine();
            bool resign = ImGui::Button("Resign");

            if(pass && cs.ready_to_make_move) {
                bool made_move = gd.pass();
                if(made_move) {
                    cs.ready_to_make_move = false;
                    send_last_move(&cs.connection, &gd);
                }
            }
            if(resign && cs.ready_to_make_move) {
                bool made_move = gd.resign();
                if(made_move) {
                    cs.ready_to_make_move = false;
                    send_last_move(&cs.connection, &gd);
                }
            }


            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            ImVec2 p = ImGui::GetCursorScreenPos();
            p.x -= 10.f;
            p.y -= 10.f;

            float dim = 500.f;
            draw_board_interactive_online(&cs, draw_list, &gd, p, dim);

            
            float black_points = 0;
            float white_points = 0;
            auto w = gd.winner(&black_points, &white_points);
            if(w) ImGui::OpenPopup("Game finished");
            if(ImGui::BeginPopupModal("Game finished", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
                cs.ready_to_make_move = false;
                ImGui::Text("%s player won!", (w == STONE_WHITE) ? "White" : "Black");
                ImGui::Text("Black: %g points", black_points);
                ImGui::Text("White: %g points", white_points);
                if(ImGui::Button("Close")) {
                    the_game_is_on = false;
                    gd = {};
                    auto con = cs.connection;
                    cs = {};
                    cs.connection = con;
                    Request r = {};
                    r.type = REQUEST_LEAVE_ROOM;
                    send_request_async(&cs.connection, r);
                    ImGui::CloseCurrentPopup();
                }

                ImGui::EndPopup();
            }

            ImGui::End();
        }

        static bool show_game_list = false;
        if(show_game_list) {
            ImGui::SetNextWindowSize(ImVec2(230, 650));
            ImGui::Begin("Rooms list", &show_game_list, 0);
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            for(int i = 0; i < (int)cs.games.size(); i++) {
                ImGui::Text("Name: %s", cs.names[i].c_str());
                if(cs.can_join[i] && !the_game_is_on) {
                    ImGui::SameLine(170.f);
                    char button_id[16] = {};
                    sprintf(button_id, "Join###%d", i);
                    if(ImGui::Button(button_id)) {
                        printf("requesting join %d\n", cs.room_ids[i]);
                        gd.board.size = cs.games[i].size;
                        Request r = {};
                        r.type = REQUEST_JOIN_ROOM;
                        r.join_room.room_id = cs.room_ids[i];
                        send_request_async(&cs.connection, r);
                        show_game_list = false;
                    }
                }
                ImVec2 p = ImGui::GetCursorScreenPos();
                draw_board(draw_list, &cs.games[i], p, 200.f);
                p.y += 230.f;
                ImGui::SetCursorScreenPos(p);
            }
            ImGui::End();
        }

        // server interaction window
        {
            ImGui::Begin("Server");
            ImGui::Text("connection descriptor: %x", cs.connection.desc);
            static char server_address[16] = "localhost";
            static int server_port = 1234;
            ImGui::InputText("server address", server_address, 16);
            ImGui::InputInt("server port", &server_port);
            if(ImGui::Button("Connect")) {
                cs.connection.desc = connect_to_server(server_address, (uint16_t)server_port);
                if(cs.connection.desc) {
                    pthread_t thread;
                    pthread_create(&thread, 0, client_thread, (void *)&cs);
                }
            }

            if(ImGui::Button("List rooms")) {
                Request r = {};
                r.type = REQUEST_LIST_ROOMS;
                send_request_async(&cs.connection, r);
            }
            if(cs.got_game_list) {
                cs.got_game_list = false;
                show_game_list = true;
            }

            static char room_name[16] = "";
            ImGui::InputText("room name", room_name, 16);
            static int board_size = 9;
            ImGui::InputInt("board size", &board_size);
            if(ImGui::Button("Request new room")) {
                Request r = {};
                r.type = REQUEST_NEW_ROOM;
                r.new_room.board_size = board_size;
                gd.board.size = board_size;
                printf("requested board size %d\n", board_size);
                memcpy(&r.new_room.name, room_name, 16);
                send_request_async(&cs.connection, r);
            }
            if(cs.got_room_id && cs.room_id != 0) {
                cs.got_room_id = false;
                puts("got room id");
                ImGui::OpenPopup("Waiting for join");
            }
            if(ImGui::BeginPopupModal("Waiting for join", 0, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Waiting for the other player to join.");
                if(cs.player_joined) {
                    cs.player_joined = false;
                    the_game_is_on = true;
                    cs.ready_to_make_move = true;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Text("Game id: %d", cs.room_id);

#if 0
            static char room_id_buffer[4];
            ImGui::InputText("requested room id", room_id_buffer, 4);
            if(ImGui::Button("Request join room")) {
                Request r = {};
                r.type = REQUEST_JOIN_ROOM;
                r.join_room.room_id = atoi(room_id_buffer);
                send_request_async(&cs.connection, r);
            }
#endif
            if(cs.got_join_result && cs.join_result) {
                cs.got_join_result = false;
                the_game_is_on = true;
            }
            if(cs.other_player_left) {
                cs.other_player_left = false;
                the_game_is_on = false;
                gd = {};
                ImGui::OpenPopup("other player left");
            }
            bool dummy_bool;
            if(ImGui::BeginPopupModal("other player left", &dummy_bool)) {
                ImGui::Text("The other player has left the game.");
                ImGui::EndPopup();
            }

            if(cs.update_game_data) {
                cs.update_game_data = false;
                memcpy(&gd, &cs.game_data, sizeof(GameData));
            }
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
