# ASCII Video Player

Plays a video file as real-time ASCII art directly in the terminal, using OpenCV for decoding/processing and raw ANSI escape codes for terminal rendering.

## Features

- Real-time video-to-ASCII conversion (grayscale + histogram equalization for contrast)
- Decouples video playback rate from render/redraw rate (60fps redraw regardless of source video fps)
- Loops playback automatically when the video ends
- Runs in the terminal's alternate screen buffer (won't clutter your shell history)
- Clean exit on Ctrl+C / SIGTERM

## Requirements

- macOS (uses `_NSGetExecutablePath` from `<mach-o/dyld.h>` — not portable to Linux/Windows as-is)
- OpenCV 5.x (only `core`, `imgproc`, and `videoio` modules are needed)
- A C++23-capable compiler (g++ or clang++)

## Build

```bash
g++ -std=c++23 -O2 src/main.cpp -o ascii_player \
  -I$(brew --prefix opencv)/include/opencv4 \
  -L$(brew --prefix opencv)/lib \
  -lopencv_core -lopencv_imgproc -lopencv_videoio
```

> Avoid linking with `pkg-config --cflags --libs opencv5` — it pulls in the entire OpenCV package (57+ modules including `dnn`, `stitching`, `viz`, etc.), none of which this project uses. Linking only the three modules above keeps the binary lean and avoids unnecessary dependencies.

## Run

Place a video file named `vid.mp4` in the **same directory as the compiled binary**, then run:

```bash
./ascii_player
```

The player looks for `vid.mp4` next to the executable, not relative to your current working directory.

## Distributing to others

The compiled binary dynamically links against Homebrew's OpenCV at an absolute path (`/opt/homebrew/...`), so it won't run out of the box on a machine without the exact same OpenCV install. To share a working binary with someone else:

1. Install [`dylibbundler`](https://github.com/auriamg/macdylibbundler): `brew install dylibbundler`
2. Bundle the dependencies:
   ```bash
   dylibbundler -od -b -x ./ascii_player -d ./libs -p @executable_path/libs/
   ```
3. Ship `ascii_player`, the `libs/` folder, and `vid.mp4` together as one folder/zip.

The recipient will need to right-click → Open on first launch, since the bundled binary is ad-hoc signed and macOS Gatekeeper will flag it as being from an unidentified developer.

## Configuration

Constants at the top of `src/main.cpp` control output size and framerate:

```cpp
constexpr int ASCII_WIDTH = 100;
constexpr int ASCII_HEIGHT = 40;
constexpr double TARGET_FPS = 60.0;
```

## License

_(add your license here, e.g. MIT)_