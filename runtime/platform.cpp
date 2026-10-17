// Host platform: SDL2 window with an OpenGL 3.3 context, audio queueing, controllers and keyboard.
#include "platform.h"
#include "dcs.h"
#include "gl.h"
#include "runtime.h"
#include "board.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#define GL_DEFINE(type, name) type name;
GL_FUNCS(GL_DEFINE)
#undef GL_DEFINE

bool gl_load() {
#define GL_LOAD(type, name)                                                                                \
    name = (type)SDL_GL_GetProcAddress(strncmp(#name, "rt_", 3) ? #name : #name + 3);                       \
    if (!name) {                                                                                           \
        rt::log("OpenGL function %s is missing", #name);                                                   \
        return false;                                                                                      \
    }
    GL_FUNCS(GL_LOAD)
#undef GL_LOAD
    return true;
}

namespace rt {

bool ide_shutdown();

void shutdown() {
    board_shutdown();
    ide_shutdown();
}

namespace {

SDL_Window *window;
SDL_GLContext context;
SDL_AudioDeviceID audio;
SDL_GameController *pad;
bool quit;
Controls controls;
bool headless_mode;

GLuint present_program, present_vao, present_tex;

GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[4096];
        glGetShaderInfoLog(s, sizeof(buf), nullptr, buf);
        fatal("shader: %s", buf);
    }
    return s;
}

void setup_present() {
    const char *vs = R"(#version 330 core
        out vec2 uv;
        uniform vec4 rect; // x, y, w, h in clip space
        void main() {
            vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
            uv = vec2(p.x, 1.0 - p.y);
            gl_Position = vec4(rect.xy + p * rect.zw, 0.0, 1.0);
        })";
    const char *fs = R"(#version 330 core
        in vec2 uv;
        out vec4 color;
        uniform sampler2D image;
        void main() { color = vec4(texture(image, uv).rgb, 1.0); })";
    present_program = glCreateProgram();
    glAttachShader(present_program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(present_program, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(present_program);
    glGenVertexArrays(1, &present_vao);
    glGenTextures(1, &present_tex);
    glBindTexture(GL_TEXTURE_2D, present_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void open_pad() {
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) {
            pad = SDL_GameControllerOpen(i);
            if (pad) {
                log("controller: %s", SDL_GameControllerName(pad));
                return;
            }
        }
}

float axis(SDL_GameControllerAxis a) {
    if (!pad) return 0;
    return SDL_GameControllerGetAxis(pad, a) / 32767.0f;
}
bool button(SDL_GameControllerButton b) { return pad && SDL_GameControllerGetButton(pad, b); }

void apply_script(Controls &c);

void read_controls() {
    const Uint8 *k = SDL_GetKeyboardState(nullptr);
    Controls c;
    // Steering: left stick, or the arrow keys.
    float s = axis(SDL_CONTROLLER_AXIS_LEFTX);
    if (std::fabs(s) < 0.08f) s = 0;
    if (k[SDL_SCANCODE_LEFT]) s = -1;
    if (k[SDL_SCANCODE_RIGHT]) s = 1;
    c.steering = std::clamp(s, -1.0f, 1.0f);
    // Pedals: triggers (or A / B), or Up / Down.
    c.accel = std::max(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT), button(SDL_CONTROLLER_BUTTON_A) ? 1.0f : 0.0f);
    c.brake = std::max(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT), button(SDL_CONTROLLER_BUTTON_B) ? 1.0f : 0.0f);
    if (k[SDL_SCANCODE_UP]) c.accel = 1;
    if (k[SDL_SCANCODE_DOWN]) c.brake = 1;
    c.start = button(SDL_CONTROLLER_BUTTON_START) || k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_1];
    c.coin = button(SDL_CONTROLLER_BUTTON_BACK) || k[SDL_SCANCODE_5];
    c.test = k[SDL_SCANCODE_F2];
    c.service = k[SDL_SCANCODE_9];
    c.view1 = button(SDL_CONTROLLER_BUTTON_X) || k[SDL_SCANCODE_Q];
    c.view2 = button(SDL_CONTROLLER_BUTTON_Y) || k[SDL_SCANCODE_W];
    c.view3 = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) || k[SDL_SCANCODE_E];
    c.radio = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || k[SDL_SCANCODE_R];
    if (button(SDL_CONTROLLER_BUTTON_DPAD_UP) || k[SDL_SCANCODE_A]) c.gear = 1;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || k[SDL_SCANCODE_S]) c.gear = 2;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || k[SDL_SCANCODE_D]) c.gear = 3;
    if (button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || k[SDL_SCANCODE_F]) c.gear = 4;
    apply_script(c);
    c.accel = std::clamp(c.accel, 0.0f, 1.0f);
    c.brake = std::clamp(c.brake, 0.0f, 1.0f);
    controls = c;
}

// Scripted input for testing: ARC_INPUT="30:coin,32:start,40:accel:20" (seconds:control[:duration]).
struct ScriptEvent {
    double at, duration;
    std::string control;
};
std::vector<ScriptEvent> script;

void load_script() {
    const char *s = getenv("ARC_INPUT");
    if (!s) return;
    std::string all = s;
    size_t pos = 0;
    while (pos < all.size()) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        std::string item = all.substr(pos, end - pos);
        ScriptEvent e{0, 0.3, ""};
        size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
        if (c1 != std::string::npos) {
            e.at = atof(item.substr(0, c1).c_str());
            e.control = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
            if (c2 != std::string::npos) e.duration = atof(item.substr(c2 + 1).c_str());
            script.push_back(e);
        }
        pos = end + 1;
    }
}

void apply_script(Controls &c) {
    double t = now_ns() / 1e9;
    for (const ScriptEvent &e : script) {
        if (t < e.at || t >= e.at + e.duration) continue;
        const std::string &n = e.control;
        if (n == "coin") c.coin = true;
        else if (n == "start") c.start = true;
        else if (n == "test") c.test = true;
        else if (n == "accel") c.accel = 1;
        else if (n == "brake") c.brake = 1;
        else if (n == "left") c.steering = -1;
        else if (n == "right") c.steering = 1;
        else if (n == "view1") c.view1 = true;
        else if (n == "view2") c.view2 = true;
        else if (n == "view3") c.view3 = true;
        else if (n == "radio") c.radio = true;
        else if (n.size() == 5 && n.rfind("gear", 0) == 0) c.gear = n[4] - '0';
    }
}

void queue_audio() {
    static int16_t buf[31250 * 2];
    size_t frames = dcs_take_audio(buf, 31250);
    if (!audio || !frames) return;
    // Keep the latency bounded: drop audio when more than ~100 ms is queued.
    if (SDL_GetQueuedAudioSize(audio) > 31250 * 4 / 10) return;
    SDL_QueueAudio(audio, buf, (Uint32)(frames * 4));
}

} // namespace

bool platform_init(bool headless) {
    headless_mode = headless;
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | (headless ? 0 : SDL_INIT_VIDEO)) != 0) {
        log("SDL_Init: %s", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 31250;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    else log("audio: %s", SDL_GetError());
    open_pad();
    load_script();
    if (headless) return true;

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window = SDL_CreateWindow("California Speed (arcade)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1024, 768,
                              SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        log("window: %s", SDL_GetError());
        return false;
    }
    context = SDL_GL_CreateContext(window);
    if (!context || !gl_load()) return false;
    SDL_GL_SetSwapInterval(0); // the game's own timing paces the frames
    setup_present();
    return true;
}

void platform_frame() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: quit = true; break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!pad) open_pad();
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (pad && !SDL_GameControllerGetAttached(pad)) {
                SDL_GameControllerClose(pad);
                pad = nullptr;
                open_pad();
            }
            break;
        }
    }
    read_controls();
    queue_audio();
    if (quit) {
        shutdown();
        exit(0);
    }
}

void platform_present_565(const uint16_t *pixels, int width, int height, int stride) {
    if (headless_mode || !window) return;
    glBindTexture(GL_TEXTURE_2D, present_tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, width, height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, pixels);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    int ww, wh;
    SDL_GL_GetDrawableSize(window, &ww, &wh);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    // 4:3, letterboxed
    float aspect = (float)ww / wh, w = 2, h = 2;
    if (aspect > 4.0f / 3) w = 2 * (4.0f / 3) / aspect;
    else h = 2 * aspect / (4.0f / 3);
    glUseProgram(present_program);
    glUniform4f(glGetUniformLocation(present_program, "rect"), -w / 2, -h / 2, w, h);
    glBindVertexArray(present_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    SDL_GL_SwapWindow(window);
}

bool platform_quit_requested() { return quit; }

void platform_window_size(int &w, int &h) {
    if (window) SDL_GL_GetDrawableSize(window, &w, &h);
    else w = h = 1;
}

void platform_swap() {
    if (window) SDL_GL_SwapWindow(window);
}
const Controls &platform_controls() { return controls; }

} // namespace rt
