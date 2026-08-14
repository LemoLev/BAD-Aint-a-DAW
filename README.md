# BAD Ain't a DAW

BAD is a simple DAW (Digital Audio Workstation) written in C with Raylib as its core library for video and audio output.

Remarkable features:
- Recording/Playback (<kbd>TAB</kbd>)
- Metronome (toggleable with <kbd>Ctrl</kbd>+<kbd>M</kbd>, on by default)
## Compilation
Since the whole program is a single file (main.c), this command will be enough:

For Linux
```bash
cc -o main main.c -O1 -ggdb -lX11 -lraylib -lm -Wall -Wextra
```

If using MinGW for compilation to Windows with GCC, then you can just omit the `-lX11` flag:
```bash
cc -o main main.c -O1 -ggdb -lraylib -lm -Wall -Wextra
```
