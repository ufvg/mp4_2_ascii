#include <opencv2/opencv.hpp>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <csignal>
#include <filesystem>
#include <mach-o/dyld.h>   // macOS-only header, needed for _NSGetExecutablePath below

constexpr int ASCII_WIDTH = 100;
constexpr int ASCII_HEIGHT = 40;
constexpr double TARGET_FPS = 60.0;   // how fast WE redraw the terminal, independent of the video's own fps

// sig_atomic_t + volatile: required combo for a flag that's safely readable/writable
// from inside a signal handler (SIGINT/SIGTERM) without undefined behavior.
volatile sig_atomic_t running = 1;

void signalHandler(int)
{
    running = 0; // just flips the flag; main loop checks it and exits cleanly
}

// --- ANSI escape codes for terminal control ---
// \033 is ESC. These sequences are interpreted by the terminal emulator, not printed as text.

void clearTerminal()
{
    std::cout << "\033[2J\033[H"; // 2J = clear entire screen, H = move cursor to top-left (1,1)
}

void moveCursorHome()
{
    std::cout << "\033[H"; // move cursor to top-left WITHOUT clearing — avoids flicker between frames
}

void hideCursor()
{
    std::cout << "\033[?25l"; // ?25l = hide the blinking text cursor so it doesn't visually clash with ASCII frames
}

void showCursor()
{
    std::cout << "\033[?25h"; // ?25h = restore the cursor on exit
}

void enableAlternativeScreen()
{
    std::cout << "\033[?1049h"; // switches to a separate terminal buffer (like vim/less does),
                                 // so playback doesn't scroll/pollute the user's normal shell history
}

void disableAlternativeScreen()
{
    std::cout << "\033[?1049l"; // restores the user's original terminal contents on exit
}

/* Resolves the folder the compiled binary lives in, so vid.mp4 can be found
next to the executable regardless of the caller's current working directory.
_NSGetExecutablePath is macOS-specific (from <mach-o/dyld.h>) — no direct
equivalent on Linux/Windows, this won't port as-is.*/
std::filesystem::path getExecutableDirectory()
{
    uint32_t size = 0;

    /*First call with a null buffer: macOS fills `size` with the required
     buffer length instead of the path itself. This is the documented way
     to query the length before allocating.*/
    _NSGetExecutablePath(nullptr, &size);

    std::string path(size, '\0');

    // Second call with a correctly sized buffer actually retrieves the path.
    if (_NSGetExecutablePath(path.data(), &size) != 0)
    {
        throw std::runtime_error("Could not determine executable path");
    }

    // canonical() resolves any symlinks (e.g. if the binary was launched via
    // a symlinked path) so we get the real directory.
    return std::filesystem::canonical(path).parent_path();
}

int main()
{
    std::filesystem::path videoPath;

    try
    {
        videoPath = getExecutableDirectory() / "vid.mp4"; // video must sit next to the compiled binary
    }
    catch (const std::exception& e)
    {
        std::cerr << "ERROR: " << e.what() << '\n';
        return 1;
    }

    cv::VideoCapture video(videoPath.string());

    if (!video.isOpened())
    {
        std::cerr << "ERROR: Could not open video: "
                  << videoPath << '\n';
        return 1;
    }

    double videoFPS = video.get(cv::CAP_PROP_FPS);
    int originalWidth = static_cast<int>(video.get(cv::CAP_PROP_FRAME_WIDTH));
    int originalHeight = static_cast<int>(video.get(cv::CAP_PROP_FRAME_HEIGHT));
    int frameCount = static_cast<int>(video.get(cv::CAP_PROP_FRAME_COUNT));

    if (videoFPS <= 0) videoFPS = 30.0; // guard against files with missing/bad fps metadata

    // How long (in seconds) each *video* frame should be displayed for,
    // based on the source file's own frame rate.
    const auto videoFrameDuration =
        std::chrono::duration<double>(1.0 / videoFPS);

    /* How long each *render* loop iteration should take to hit TARGET_FPS.
    Kept separate from videoFrameDuration because the two rates can differ
    a 24fps video can still be redrawn/refreshed at 60fps.*/
    constexpr auto targetFrameDuration =
        std::chrono::duration<double>(1.0 / TARGET_FPS);

    std::cout << "Video: " << originalWidth << "x" << originalHeight << '\n';
    std::cout << "Original FPS: " << videoFPS << '\n';
    std::cout << "Frames: " << frameCount << '\n';
    std::cout << "Output FPS: " << TARGET_FPS << '\n';
    std::cout << "Video path: " << videoPath << '\n';
    std::cout << "Starting in 2 seconds...\n";

    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::signal(SIGINT, signalHandler);  // Ctrl+C
    std::signal(SIGTERM, signalHandler); // e.g. `kill` command

    enableAlternativeScreen();
    hideCursor();
    clearTerminal();

    /* Disabling C stdio sync and untying cin from cout is a standard perf
    optimization for tight I/O loops — avoids per-write sync overhead we
    don't need since we're not mixing printf/scanf with cout/cin here.*/
    std::ios_base::sync_with_stdio(false);
    std::cin.tie(nullptr);

    /* Brightness ramp from darkest to brightest. Each entry is printed TWICE
    per pixel (e.g. "@@" not "@") to compensate for terminal character
    cells being roughly twice as tall as they are wide — without doubling,
    the image looks squashed horizontally.*/
    const char* charset[] = {
        "  ",
        "..",
        "::",
        ";;",
        "++",
        "**",
        "??",
        "%%",
        "##",
        "@@"
    };

    // Precomputed lookup table: maps every possible 0-255 grayscale pixel
    // value directly to its ASCII string, so the per-pixel hot loop below
    // does a plain array index instead of recalculating `i * 10 / 256` and
    // an array lookup for every single pixel of every frame.
    std::string charMap[256];

    for (int i = 0; i < 256; ++i)
    {
        int index = i * 10 / 256; // scales 0-255 down to the 0-9 charset range
        charMap[i] = charset[index];
    }

    cv::Mat frame, gray, resized;

    std::string frameBuffer;
    // Pre-reserve capacity for one full ASCII frame (2 chars/pixel + newline
    // per row + slack) so frameBuffer.clear()/append cycles below don't
    // trigger reallocations every frame.
    frameBuffer.reserve(
        ASCII_WIDTH * ASCII_HEIGHT * 2 +
        ASCII_HEIGHT +
        100
    );

    auto videoStartTime = std::chrono::steady_clock::now();

    if (!video.read(frame))
    {
        std::cerr << "ERROR: Could not read first frame\n";
        return 1;
    }

    while (running)
    {
        auto frameStart = std::chrono::steady_clock::now();
        auto videoElapsed = frameStart - videoStartTime;

        // Video-clock catch-up loop: advances the decoded frame forward until
        // it matches how much wall-clock time has actually passed. This decouples
        // video playback speed from render loop speed — if TARGET_FPS (60) is
        // higher than videoFPS (say 24), most render iterations will redraw the
        // same frame without reading a new one; if the render loop stalls, this
        // loop can read/skip multiple frames in one iteration to catch back up.
        while (videoElapsed >= videoFrameDuration && running)
        {
            if (!video.read(frame))
            {
                // End of file reached: loop back to the start.
                video.set(cv::CAP_PROP_POS_FRAMES, 0);
                videoStartTime = std::chrono::steady_clock::now();
                video.read(frame);
            }

            // Advance the reference clock by exactly one video-frame's worth
            // of time (rather than "now") to avoid drift from accumulating
            // processing delays each iteration.
            videoStartTime +=
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration
                >(videoFrameDuration);

            videoElapsed =
                std::chrono::steady_clock::now() - videoStartTime;
        }

        if (!running) break;

        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);   // ASCII brightness only needs luminance, not color
        cv::equalizeHist(gray, gray);                     // boosts contrast so mid-tones spread across the charset better

        // Downscale to exactly ASCII_WIDTH x ASCII_HEIGHT — one pixel maps to
        // one terminal character cell. INTER_AREA is used because it's the
        // preferred OpenCV interpolation method for shrinking images (better
        // anti-aliasing/averaging than nearest/linear when downsampling).
        cv::resize(
            gray,
            resized,
            cv::Size(ASCII_WIDTH, ASCII_HEIGHT),
            0,
            0,
            cv::INTER_AREA
        );

        frameBuffer.clear();

        for (int y = 0; y < ASCII_HEIGHT; ++y)
        {
            // ptr<unsigned char>(y) gives direct access to row y's raw pixel
            // bytes — faster than cv::Mat::at() for a tight per-pixel loop.
            const unsigned char* row =
                resized.ptr<unsigned char>(y);

            for (int x = 0; x < ASCII_WIDTH; ++x)
            {
                frameBuffer += charMap[row[x]]; // O(1) lookup instead of recomputing the charset index
            }

            frameBuffer += '\n';
        }

        // Overwrite the previous frame in place rather than clearing+redrawing,
        // which avoids the flicker a full clearTerminal() would cause every frame.
        moveCursorHome();

        std::cout << frameBuffer;
        std::cout.flush();

        // Frame pacing: measure how long the decode+render work above actually
        // took, then sleep off whatever's left of the target frame budget so
        // we hold steady at TARGET_FPS instead of running as fast as possible.
        auto processingTime =
            std::chrono::steady_clock::now() - frameStart;

        auto remaining =
            targetFrameDuration - processingTime;

        if (remaining.count() > 0)
        {
            std::this_thread::sleep_for(
                std::chrono::duration_cast<
                    std::chrono::nanoseconds
                >(remaining)
            );
        }
        // if remaining <= 0, we're already behind schedule — skip sleeping
        // and let the next iteration start immediately (no negative sleep).
    }

    disableAlternativeScreen();
    showCursor();
    std::cout << '\n';

    return 0;
}