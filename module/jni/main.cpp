#include "esp.h"
#include "offsets.h"
#include "zygisk.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <math.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/mman.h>
#include <sys/stat.h>

using namespace zygisk;

Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
std::string read_string(void* il2cpp_str);

// gl.cpp
bool  gl_init();
void  gl_set_screen_size(int w, int h);
void  gl_draw_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_text(float x, float y, const char* text, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
float gl_text_width(const char* text, float scale);

static bool g_is_target = false;
static std::atomic<bool> g_esp_on{true};
static std::atomic<int>  g_egl_count{0};
static std::atomic<int>  g_vk_count{0};
static int g_screen_w = 1080, g_screen_h = 2400;
static bool g_gl_ready = false;

// === Button koordinatları (dokunmatik için) ===
static const int BTN_X1 = 20;
static const int BTN_Y1 = 200;
static const int BTN_X2 = 220;
static const int BTN_Y2 = 320;

// === lib base ===
static uintptr_t find_lib_base(const char* libname) {
    struct Ctx { const char* n; uintptr_t out; } ctx { libname, 0 };
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        if (info->dlpi_name && strstr(info->dlpi_name, c->n)) {
            c->out = (uintptr_t)info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &ctx);
    return ctx.out;
}

// === ARM64 hook ===
static void* install_arm64_hook(void* target, void* hook_fn) {
    uint32_t* tcode = (uint32_t*)target;
    void* tramp = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
    uint32_t* tc = (uint32_t*)tramp;
    for (int i = 0; i < 4; i++) tc[i] = tcode[i];
    tc[4] = 0x58000050;
    tc[5] = 0xD61F0200;
    *(uint64_t*)&tc[6] = (uint64_t)target + 16;
    uintptr_t page = (uintptr_t)target & ~0xFFFULL;
    mprotect((void*)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    tcode[0] = 0x58000050;
    tcode[1] = 0xD61F0200;
    *(uint64_t*)&tcode[2] = (uint64_t)hook_fn;
    mprotect((void*)page, 4096, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)target, (char*)target + 16);
    return tramp;
}

// === EGL hook ===
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t g_orig_egl = nullptr;

static void draw_esp();
static void draw_menu();
static void draw_heartbeat_marker();

static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    int c = g_egl_count.fetch_add(1) + 1;
    if (c == 1) LOGI(">>> eglSwapBuffers ILK ÇAĞRI <<<");
    if (c == 30) LOGI(">>> eglSwapBuffers 30 çağrı <<<");
    if (c % 300 == 1) LOGI("eglSwapBuffers count=%d", c);

    if (!g_gl_ready) {
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        if (w > 0 && h > 0) {
            g_screen_w = w;
            g_screen_h = h;
            if (gl_init()) {
                g_gl_ready = true;
                LOGI("GL init OK: %dx%d", w, h);
            }
        }
    }
    if (g_gl_ready) {
        glViewport(0, 0, g_screen_w, g_screen_h);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_heartbeat_marker();
        draw_menu();
        if (g_esp_on.load()) draw_esp();
    }
    return g_orig_egl(dpy, surf);
}

// === Vulkan hook ===
typedef int (*vkQueuePresentKHR_t)(void*, const void*);
static vkQueuePresentKHR_t g_orig_vk = nullptr;

static int my_vkQueuePresentKHR(void* q, const void* info) {
    int c = g_vk_count.fetch_add(1) + 1;
    if (c == 1) LOGI(">>> vkQueuePresentKHR ILK ÇAĞRI — OYUN VULKAN! <<<");
    if (c == 30) LOGI(">>> vkQueuePresentKHR 30 çağrı <<<");
    if (c % 300 == 1) LOGI("vkQueuePresentKHR count=%d", c);
    return g_orig_vk(q, info);
}

// === Heartbeat thread ===
static void heartbeat_thread() {
    for (int i = 0; i < 600; i++) {
        sleep(2);
        LOGI("[heartbeat] egl=%d vk=%d gl_ready=%d esp=%d",
             g_egl_count.load(), g_vk_count.load(),
             (int)g_gl_ready, (int)g_esp_on.load());
    }
}

// === Touch input reader (dokunmatik ile ESP toggle) ===
static void touch_thread() {
    sleep(8); // Oyun başlaması için bekle

    // Touchscreen device bul
    char evdev[64] = {0};
    FILE* f = fopen("/proc/bus/input/devices", "r");
    if (f) {
        char line[512];
        bool is_touch = false;
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "Touchscreen") || strstr(line, "touchscreen") ||
                strstr(line, "touch")) {
                is_touch = true;
            }
            if (is_touch && strstr(line, "event")) {
                char* p = strstr(line, "event");
                if (p) {
                    int n = 0;
                    sscanf(p, "event%d", &n);
                    snprintf(evdev, sizeof(evdev), "/dev/input/event%d", n);
                    break;
                }
            }
        }
        fclose(f);
    }

    if (evdev[0] == 0) {
        // Fallback - event0..event5 dene
        for (int i = 0; i < 6; i++) {
            snprintf(evdev, sizeof(evdev), "/dev/input/event%d", i);
            if (access(evdev, R_OK) == 0) break;
            evdev[0] = 0;
        }
    }
    if (evdev[0] == 0) { LOGE("touch device bulunamadi"); return; }
    LOGI("touch device: %s", evdev);

    int fd = open(evdev, O_RDONLY);
    if (fd < 0) { LOGE("touch open basarisiz"); return; }

    int last_x = -1, last_y = -1;
    struct input_event ev;
    while (read(fd, &ev, sizeof(ev)) == sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) last_x = ev.value;
            if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) last_y = ev.value;
        }
        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 0) {
            // Parmak kalktı
            if (last_x > BTN_X1 && last_x < BTN_X2 && last_y > BTN_Y1 && last_y < BTN_Y2) {
                bool old = g_esp_on.load();
                g_esp_on.store(!old);
                LOGI(">>> ESP TOGGLE: %s <<<", !old ? "ON" : "OFF");
            }
        }
    }
    close(fd);
}

// === EntityManager.Instance ===
static void* get_entity_manager_instance() {
    typedef void* (*t)();
    t fn = call_rva<t>(RVA_EntityManager_get_Instance);
    return fn();
}
static void* get_main_camera() {
    typedef void* (*t)();
    t fn = call_rva<t>(RVA_Camera_get_main);
    return fn();
}

// === World to Screen ===
struct W2SOut { float x, y, z; };
static bool w2s(void* cam, Vec3 w, Vec2* o) {
    if (!cam || !o) return false;
    typedef W2SOut (*t)(void*, Vec3);
    t fn = (t)call_rva<void*>(RVA_Camera_WorldToScreenPoint);
    W2SOut r = fn(cam, w);
    o->x = r.x; o->y = r.y;
    return r.z > 0;
}

// === ESP çiz ===
static void draw_esp() {
    void* cam = get_main_camera();
    void* em = get_entity_manager_instance();
    if (!cam || !em) return;
    void* dict = *(void**)((uint8_t*)em + 0x18);
    if (!dict) return;
    void* arr = *(void**)((uint8_t*)dict + 0x18);
    int32_t cnt = *(int32_t*)((uint8_t*)dict + 0x20);
    if (!arr || cnt <= 0 || cnt > 10000) return;

    Vec3 local = {0,0,0};
    bool lf = false;
    uint8_t* base = (uint8_t*)arr + 0x20;

    for (int i = 0; i < cnt; i++) {
        uint8_t* e = base + i*0x18;
        int32_t h = *(int32_t*)(e+0x00);
        void* v = *(void**)(e+0x10);
        if (h == -1 || !v) continue;
        if (*(void**)v != g_player_class) continue;
        typedef bool (*gb)(void*);
        gb isLocal = call_rva<gb>(RVA_Player_get_bLocalPlayer);
        if (isLocal(v)) {
            typedef float (*gf)(void*);
            gf px=call_rva<gf>(RVA_Player_get_PosX_Smooth);
            gf py=call_rva<gf>(RVA_Player_get_PosY_Smooth);
            gf pz=call_rva<gf>(RVA_Player_get_PosZ_Smooth);
            local.x=px(v); local.y=py(v); local.z=pz(v);
            lf = true; break;
        }
    }

    for (int i = 0; i < cnt; i++) {
        uint8_t* e = base + i*0x18;
        int32_t h = *(int32_t*)(e+0x00);
        void* v = *(void**)(e+0x10);
        if (h == -1 || !v) continue;
        if (*(void**)v != g_player_class) continue;

        typedef bool (*gb)(void*);
        gb isLocal = call_rva<gb>(RVA_Player_get_bLocalPlayer);
        if (isLocal(v)) continue;
        gb isDead = call_rva<gb>(RVA_Player_get_IsDead);
        if (isDead(v)) continue;

        typedef float (*gf)(void*);
        gf px=call_rva<gf>(RVA_Player_get_PosX_Smooth);
        gf py=call_rva<gf>(RVA_Player_get_PosY_Smooth);
        gf pz=call_rva<gf>(RVA_Player_get_PosZ_Smooth);
        Vec3 p; p.x=px(v); p.y=py(v); p.z=pz(v);

        gf ghp = call_rva<gf>(RVA_Player_get_Hp);
        gf gmh = call_rva<gf>(RVA_Player_get_MaxHp);
        float hp = ghp(v), mhp = gmh(v);

        typedef void* (*gs)(void*);
        gs gn = call_rva<gs>(RVA_Player_get_Name);
        std::string nm = read_string(gn(v));

        Vec2 s;
        if (!w2s(cam, p, &s)) continue;
        if (s.x < 0 || s.x > g_screen_w || s.y < 0 || s.y > g_screen_h) continue;

        float dist = 0;
        if (lf) {
            float dx=p.x-local.x, dy=p.y-local.y, dz=p.z-local.z;
            dist = sqrtf(dx*dx+dy*dy+dz*dz);
        }
        float bh = 8000.0f / (dist + 1.0f);
        float bw = bh * 0.5f;
        float bx = s.x - bw * 0.5f;
        float by = s.y - bh;

        gl_draw_rect(bx, by, bw, bh, 255, 0, 0, 255);
        if (!nm.empty()) {
            float tw = gl_text_width(nm.c_str(), 1.5f);
            gl_draw_text(s.x - tw*0.5f, by - 22, nm.c_str(), 1.5f, 255,255,255,255);
        }
        if (lf) {
            char d[32]; snprintf(d, sizeof(d), "[%.0fm]", dist);
            float tw = gl_text_width(d, 1.2f);
            gl_draw_text(s.x - tw*0.5f, s.y + 4, d, 1.2f, 255,255,0,255);
        }
        if (mhp > 0) {
            float r = hp / mhp;
            if (r < 0) r = 0; if (r > 1) r = 1;
            gl_draw_filled_rect(bx-8, by, 4, bh, 0,0,0,200);
            gl_draw_filled_rect(bx-8, by + bh*(1-r), 4, bh*r, 0,255,0,255);
        }
    }
}

// === Menu çiz (buton) ===
static void draw_menu() {
    uint8_t r = g_esp_on.load() ? 0 : 200;
    uint8_t g = g_esp_on.load() ? 200 : 0;
    gl_draw_filled_rect(BTN_X1, BTN_Y1, BTN_X2-BTN_X1, BTN_Y2-BTN_Y1, r, g, 0, 200);
    gl_draw_rect(BTN_X1, BTN_Y1, BTN_X2-BTN_X1, BTN_Y2-BTN_Y1, 255, 255, 255, 255);
    gl_draw_text(BTN_X1 + 20, BTN_Y1 + 30, g_esp_on.load() ? "ESP ON" : "ESP OFF", 2.0f, 255,255,255,255);
}

// === Heartbeat göstergesi (kırmızı kare - hook çalışıyor mu?) ===
static void draw_heartbeat_marker() {
    // Sol üst köşede yeşil nokta — her frame çizilir, hook çalışıyorsa görünür
    gl_draw_filled_rect(20, 20, 40, 40, 0, 255, 0, 255);
}

// === Hook'ları kur ===
static void install_hooks() {
    void* libegl = dlopen("libEGL.so", RTLD_NOW);
    if (libegl) {
        void* t = dlsym(libegl, "eglSwapBuffers");
        if (t) {
            void* tr = install_arm64_hook(t, (void*)my_eglSwapBuffers);
            if (tr) { g_orig_egl = (eglSwapBuffers_t)tr; LOGI("EGL hook OK"); }
        }
    }
    void* libvk = dlopen("libvulkan.so", RTLD_NOW);
    if (libvk) {
        void* t = dlsym(libvk, "vkQueuePresentKHR");
        if (t) {
            void* tr = install_arm64_hook(t, (void*)my_vkQueuePresentKHR);
            if (tr) { g_orig_vk = (vkQueuePresentKHR_t)tr; LOGI("Vulkan hook OK"); }
        }
    }
}

// === Zygisk modülü ===
class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;

    void onLoad(Api* a, JNIEnv* e) override { api_ptr = a; env_ptr = e; LOGI("onLoad"); }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!api_ptr || !env_ptr || !args || !args->nice_name) {
            if (api_ptr) api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        const char* p = env_ptr->GetStringUTFChars(args->nice_name, nullptr);
        bool ok = (p && strcmp(p, "com.tencent.rmos") == 0);
        if (p) env_ptr->ReleaseStringUTFChars(args->nice_name, p);
        if (!ok) { api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY); return; }
        g_is_target = true;
        LOGI("Rust Mobile tespit edildi");
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!g_is_target) return;
        std::thread([]() {
            int a = 0;
            while (a < 60 && find_lib_base("libil2cpp.so") == 0) { sleep(1); a++; }
            if (a >= 60) { LOGE("libil2cpp yuklenmedi"); return; }
            if (!init_il2cpp_api()) return;
            LOGI("10sn bekleniyor...");
            sleep(10);
            Il2CppDomain* d = (Il2CppDomain*)api.domain_get();
            if (d && api.thread_attach) api.thread_attach(d);
            g_player_class = find_class("WizardGames.Soc.Common.Entity", "PlayerEntity");
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("WizardGames.Soc.Share.Framework", "EntityManager");
            LOGI("P=%p C=%p EM=%p", g_player_class, g_camera_class, g_entity_manager_class);
            install_hooks();
            std::thread(touch_thread).detach();
            std::thread(heartbeat_thread).detach();
            LOGI("ESP hazir!");
        }).detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)