# Go game server/client
This is a simple implementation of a server and a client for the game of Go. The protocol is built directly on top of TCP, using Unix sockets. The server is non-interactive and the client has a simple GUI utilizing Dear Imgui and SDL2.

## Compilation
To build either the client or the server simply call `make` in the appropriate directory.
```bash
$ cd client
$ make
$ cd ../server
$ make
```

The client requires a system-wide installation of SDL2 to be present on a system, to acquire it on a Debian-based distribution:
```bash
$ sudo apt-get install libsdl2-dev
```
Arch:
```bash
$ sudo pacman -S sdl2
```
