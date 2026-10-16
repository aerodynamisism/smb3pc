// Desktop front end: SDL3 window, Vulkan presenter, audio output, keyboard and
// gamepad input, and frame pacing at the NTSC rate (about 60.0988 Hz).
//
//   smb3pc [--assets DIR] [--shaders DIR] [--config FILE]
//
// Default keys: arrows = D-pad, X = A, Z = B, Enter = Start, Right Shift = Select,
// F11 = menu (fullscreen, FPS counter, audio mix), Esc = quit. Gamepad: South = A, East = B, Back = Select,
// Start = Start, D-pad = D-pad. Remap keys in settings.cfg (see README).
// Menu settings (fullscreen, FPS counter, audio mix) persist in settings.cfg.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "app/overlay.h"
#include "app/presenter.h"
#include "app/settings.h"
#include "runtime/machine.h"

#ifndef SMB3_ASSET_DIR
#define SMB3_ASSET_DIR "assets"
#endif
#ifndef SMB3_SHADER_DIR
#define SMB3_SHADER_DIR "shaders"
#endif

namespace {

// NTSC: 29780.5 CPU cycles per frame at 1789773 Hz.
constexpr uint64_t kFrameNs = 16639267;
// Audio queue target: 40 ms of mono int16, in bytes.
constexpr int kQueueTarget = static_cast<int>(Apu::kSampleRate / 25) * 2;

// Controller buttons in $4016 read order; bit i of the pad byte is button i.
enum NesButton { A, B, Select, Start, Up, Down, Left, Right, kNesButtons };
constexpr const char* kButtonNames[kNesButtons] = {"a", "b", "select", "start", "up", "down", "left", "right"};

struct Bindings {
    SDL_Scancode key[kNesButtons] = {SDL_SCANCODE_X,     SDL_SCANCODE_Z,    SDL_SCANCODE_RSHIFT,
                                     SDL_SCANCODE_RETURN, SDL_SCANCODE_UP,   SDL_SCANCODE_DOWN,
                                     SDL_SCANCODE_LEFT,  SDL_SCANCODE_RIGHT};
    SDL_GamepadButton pad[kNesButtons] = {SDL_GAMEPAD_BUTTON_SOUTH,       SDL_GAMEPAD_BUTTON_EAST,
                                          SDL_GAMEPAD_BUTTON_BACK,        SDL_GAMEPAD_BUTTON_START,
                                          SDL_GAMEPAD_BUTTON_DPAD_UP,     SDL_GAMEPAD_BUTTON_DPAD_DOWN,
                                          SDL_GAMEPAD_BUTTON_DPAD_LEFT,   SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
};

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const size_t e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// Reads lines like "key.a = X" or "pad.start = start" (names as SDL spells them).
// Missing file keeps the defaults; an unknown name is reported and rejected.
bool load_bindings(const std::string& path, Bindings& b) {
    std::ifstream f(path);
    if (!f) return true;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line)) {
        lineno++;
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const size_t eq = t.find('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "%s:%d: expected name = value\n", path.c_str(), lineno);
            return false;
        }
        const std::string name = trim(t.substr(0, eq));
        const std::string value = trim(t.substr(eq + 1));
        const bool is_key = name.rfind("key.", 0) == 0;
        const bool is_pad = name.rfind("pad.", 0) == 0;
        if (!is_key && !is_pad) continue;  // menu settings live in the same file
        const std::string button = name.substr(4);
        int idx = -1;
        for (int i = 0; i < kNesButtons; i++) {
            if (button == kButtonNames[i]) idx = i;
        }
        if (idx < 0) {
            std::fprintf(stderr, "%s:%d: unknown button '%s'\n", path.c_str(), lineno, button.c_str());
            return false;
        }
        if (is_key) {
            const SDL_Scancode sc = SDL_GetScancodeFromName(value.c_str());
            if (sc == SDL_SCANCODE_UNKNOWN) {
                std::fprintf(stderr, "%s:%d: unknown key '%s'\n", path.c_str(), lineno, value.c_str());
                return false;
            }
            b.key[idx] = sc;
        } else {
            const SDL_GamepadButton gb = SDL_GetGamepadButtonFromString(value.c_str());
            if (gb == SDL_GAMEPAD_BUTTON_INVALID) {
                std::fprintf(stderr, "%s:%d: unknown gamepad button '%s'\n", path.c_str(), lineno, value.c_str());
                return false;
            }
            b.pad[idx] = gb;
        }
    }
    return true;
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    std::string assets = SMB3_ASSET_DIR;
    std::string shaders = SMB3_SHADER_DIR;
    std::string config = "settings.cfg";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--assets" && i + 1 < argc) assets = argv[++i];
        else if (arg == "--shaders" && i + 1 < argc) shaders = argv[++i];
        else if (arg == "--config" && i + 1 < argc) config = argv[++i];
    }

    // Menu settings and controller bindings share one file (missing file keeps
    // the defaults; an old bindings-only smb3pc.cfg loads fine as well).
    AppSettings app_settings;
    settings::load(config, app_settings);
    Bindings bindings;
    if (!load_bindings(config, bindings)) return 1;

    std::vector<uint8_t> prg = read_file(assets + "/prg.bin");
    std::vector<uint8_t> chr = read_file(assets + "/chr.bin");
    if (prg.size() != 256 * 1024 || chr.size() != 128 * 1024) {
        std::fprintf(stderr, "asset pack missing or wrong size in %s\n", assets.c_str());
        return 1;
    }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // 720x720 is exactly 3x the visible 240x240 picture (overscan cropped), so the
    // pillarboxed viewport fills the window with no black border at startup.
    SDL_Window* window = SDL_CreateWindow("Super Mario Bros. 3", 720, 720,
                                          SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    if (app_settings.fullscreen) SDL_SetWindowFullscreen(window, true);

    // Mono int16 at the APU output rate. Without an audio device the game runs on the frame timer.
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16;
    spec.channels = 1;
    spec.freq = static_cast<int>(Apu::kSampleRate);
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) {
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        std::fprintf(stderr, "no audio device: %s\n", SDL_GetError());
    }

    // Open the first connected gamepad; later ones are picked up on hot-plug.
    SDL_Gamepad* pad = nullptr;
    int gp_count = 0;
    SDL_JoystickID* gp_ids = SDL_GetGamepads(&gp_count);
    if (gp_ids && gp_count > 0) pad = SDL_OpenGamepad(gp_ids[0]);
    SDL_free(gp_ids);

    int exit_code = 0;
    Presenter presenter;
    Machine* m = new Machine();
    m->prg = prg.data();
    m->chr = chr.data();
    if (!presenter.init(window, shaders)) {
        exit_code = 1;
    } else {
        m->reset();
        m->apu.set_mixing(app_settings.mixing);
        uint8_t kb_state = 0;   // keyboard contribution to the pad byte
        uint8_t gp_state = 0;   // gamepad contribution
        bool running = true;
        uint64_t deadline = SDL_GetTicksNS();
        std::vector<int16_t> samples;
        // F11 (keyboard) or L3+R3 (gamepad sticks) opens the menu. The game is paused while it is open.
        bool menu_open = false;
        bool l3_down = false;     // gamepad menu-chord state (L3+R3 toggles the menu)
        bool r3_down = false;
        bool chord_fired = false;
        int menu_sel = 0;
        bool show_fps = app_settings.show_fps;
        bool nonlinear_audio = app_settings.mixing == Mixing::Nonlinear;
        // Fullscreen is tracked here, not read back from the window: on some
        // platforms (notably Wayland) the window flags update asynchronously,
        // so a read-back right after toggling can return the old state.
        bool fullscreen = app_settings.fullscreen;
        // Menu toggles persist immediately so they survive even a crash.
        auto save_settings = [&]() {
            app_settings.fullscreen = fullscreen;
            app_settings.show_fps = show_fps;
            app_settings.mixing = nonlinear_audio ? Mixing::Nonlinear : Mixing::Linear;
            if (!settings::save(config, app_settings)) {
                std::fprintf(stderr, "cannot write %s\n", config.c_str());
            }
        };
        int fps_shown = 0;
        int fps_frames = 0;
        uint64_t fps_t0 = SDL_GetTicksNS();
        std::vector<uint32_t> screen(256 * 240);
        auto menu_action = [&](MenuAction act) {
            switch (act) {
                case MenuAction::Up:
                    menu_sel = (menu_sel + kMenuItems - 1) % kMenuItems;
                    break;
                case MenuAction::Down:
                    menu_sel = (menu_sel + 1) % kMenuItems;
                    break;
                case MenuAction::Select:
                    if (menu_sel == 0) {
                        fullscreen = !fullscreen;
                        if (!SDL_SetWindowFullscreen(window, fullscreen)) {
                            std::fprintf(stderr, "cannot set fullscreen: %s\n", SDL_GetError());
                            fullscreen = !fullscreen;
                        }
                        save_settings();
                    } else if (menu_sel == 1) {
                        show_fps = !show_fps;
                        save_settings();
                    } else if (menu_sel == 2) {
                        nonlinear_audio = !nonlinear_audio;
                        m->apu.set_mixing(nonlinear_audio ? Mixing::Nonlinear : Mixing::Linear);
                        save_settings();
                    } else {
                        menu_open = false;  // unreachable, but never trap the player
                    }
                    break;
            }
        };
        // The frame to show: the NES picture with the overlays that are on.
        auto compose = [&](const uint32_t* src) {
            std::memcpy(screen.data(), src, sizeof(uint32_t) * screen.size());
            if (show_fps) draw_fps(screen.data(), fps_shown, m->chr);
            if (menu_open)
                draw_menu(screen.data(), MenuView{menu_sel, fullscreen, show_fps, nonlinear_audio},
                          m->chr);
            return static_cast<const uint32_t*>(screen.data());
        };
        while (running) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                switch (ev.type) {
                    case SDL_EVENT_QUIT:
                        running = false;
                        break;
                    case SDL_EVENT_KEY_DOWN:
                    case SDL_EVENT_KEY_UP: {
                        const bool down = ev.type == SDL_EVENT_KEY_DOWN;
                        if (down && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_F11) {
                            menu_open = !menu_open;
                            menu_sel = 0;
                        } else if (down && ev.key.scancode == SDL_SCANCODE_ESCAPE) {
                            running = false;
                        } else if (menu_open && down && !ev.key.repeat) {
                            if (ev.key.scancode == SDL_SCANCODE_UP) {
                                menu_action(MenuAction::Up);
                            } else if (ev.key.scancode == SDL_SCANCODE_DOWN) {
                                menu_action(MenuAction::Down);
                            } else if (ev.key.scancode == SDL_SCANCODE_RETURN || ev.key.scancode == SDL_SCANCODE_X) {
                                menu_action(MenuAction::Select);
                            }
                        }
                        for (int i = 0; i < kNesButtons; i++) {
                            if (bindings.key[i] != ev.key.scancode) continue;
                            const uint8_t bit = static_cast<uint8_t>(1u << i);
                            kb_state = down ? static_cast<uint8_t>(kb_state | bit) : static_cast<uint8_t>(kb_state & ~bit);
                        }
                        break;
                    }
                    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
                        const bool down = ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_STICK) l3_down = down;
                        if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) r3_down = down;
                        if (!down) {
                            chord_fired = false;  // re-arm once either stick is released
                        } else if (l3_down && r3_down && !chord_fired) {
                            chord_fired = true;
                            menu_open = !menu_open;
                            if (menu_open) menu_sel = 0;
                        }
                        if (menu_open && down) {
                            switch (ev.gbutton.button) {
                                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                                    menu_action(MenuAction::Up);
                                    break;
                                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                                    menu_action(MenuAction::Down);
                                    break;
                                case SDL_GAMEPAD_BUTTON_SOUTH:
                                    menu_action(MenuAction::Select);
                                    break;
                                case SDL_GAMEPAD_BUTTON_EAST:
                                case SDL_GAMEPAD_BUTTON_START:
                                    menu_open = false;
                                    break;
                                default:
                                    break;
                            }
                        }
                        for (int i = 0; i < kNesButtons; i++) {
                            if (bindings.pad[i] != ev.gbutton.button) continue;
                            const uint8_t bit = static_cast<uint8_t>(1u << i);
                            gp_state = down ? static_cast<uint8_t>(gp_state | bit) : static_cast<uint8_t>(gp_state & ~bit);
                        }
                        break;
                    }
                    case SDL_EVENT_GAMEPAD_ADDED:
                        if (!pad) pad = SDL_OpenGamepad(ev.gdevice.which);
                        break;
                    case SDL_EVENT_GAMEPAD_REMOVED:
                        if (pad && SDL_GetGamepadID(pad) == ev.gdevice.which) {
                            SDL_CloseGamepad(pad);
                            pad = nullptr;
                            gp_state = 0;
                            l3_down = r3_down = chord_fired = false;
                        }
                        break;
                    default:
                        break;
                }
            }
            if (menu_open) {
                // Paused: keep showing the last frame with the menu on it.
                if (!presenter.present(compose(m->framebuf))) {
                    exit_code = 1;
                    break;
                }
                SDL_DelayNS(10000000);  // 10 ms
                continue;
            }
            m->joy[0] = static_cast<uint8_t>(kb_state | gp_state);
            // One emulated frame per presented image, so motion advances evenly.
            m->run_frame();
            fps_frames++;
            samples.clear();
            if (audio) {
                // Nudge the APU's output rate by at most 0.5%: a little fast when the device queue is
                // low, a little slow when high. This holds the queue at its target without dropping audio.
                const double fill = (static_cast<double>(kQueueTarget) - SDL_GetAudioStreamQueued(audio)) / kQueueTarget;
                m->apu.set_rate_scale(1.0 + 0.005 * std::clamp(fill, -1.0, 1.0));
            }
            m->apu.render(m->cyc, samples);  // also drains the APU write queue
            if (audio) {
                SDL_PutAudioStreamData(audio, samples.data(), static_cast<int>(samples.size() * sizeof(int16_t)));
            }
            // FPS: emulated frames per second, updated every half second.
            const uint64_t now_ns = SDL_GetTicksNS();
            if (now_ns - fps_t0 >= 500000000ull) {
                fps_shown = static_cast<int>(fps_frames * 1000000000.0 / static_cast<double>(now_ns - fps_t0) + 0.5);
                fps_frames = 0;
                fps_t0 = now_ns;
            }
            if (!presenter.present(show_fps ? compose(m->framebuf) : m->framebuf)) {
                exit_code = 1;
                break;
            }

            {
                deadline += kFrameNs;
                const uint64_t now = SDL_GetTicksNS();
                if (deadline > now) {
                    SDL_DelayNS(deadline - now);
                } else {
                    deadline = now;  // fell behind: resynchronise instead of catching up
                }
            }
        }
    }

    delete m;
    presenter.shutdown();
    if (pad) SDL_CloseGamepad(pad);
    if (audio) SDL_DestroyAudioStream(audio);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
