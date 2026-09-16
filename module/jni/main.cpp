#include "esp.h"
#include "offsets.h"
#include "zygisk.hpp"
#include "dobby.h"
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
#include <fstream>
#include <math.h>

using namespace zygisk;

// gl.cpp fonksiyonları
bool gl_init();
void gl_set_screen_size(int w, int h);
void gl_draw_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void gl_draw_text(float x, float y, const char* text, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
float gl_text_width(const char* text, float scale);

// il2cpp.cpp fonksiyonları
bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
std::string read_string(void* il2cpp_str);

// Global
Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

static bool g_is_target = false;
static bool g_gl_ready = false;
static std::atomic<bool> g_esp_on{false};
static std::atomic<int> g_egl_count{0};
static std::atomic<int> g_vk_count{0};
static int g_screen_w = 1080, g_screen_h = 2400;

#define ESP_STATE_FILE "/data/local/tmp/esp_state.txt"

// EGL hook
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t g_orig_egl = nullptr;

// Vulkan hook
typedef int (*vkQueuePresentKHR_t)(void*, const void*);
static vkQueuePresentKHR_t g_orig_vk = nullptr;

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

static void* get_entity_manager_instance() {
    typedef void* (*t)();
    t fn = call_rva<t>(RVA_EntityManager_get_Instance);
    return fn ? fn() : nullptr;
}

static void* get_main_camera() {
    typedef void* (*t)();
    t fn = call_rva<t>(RVA_Camera_get_main);
    return fn ? fn() : nullptr;
}

struct W2SOut { float x, y, z; };

static bool w2s(void* cam, Vec3 w, Vec2* o) {
    if (!cam || !o) return false;
    typedef W2SOut (*t)(void*, Vec3);
    t fn = (t)call_rva<void*>(RVA_Camera_WorldToScreenPoint);
    if (!fn) return false;
    W2SOut r = fn(cam, w);
    o->x = r.x;
    o->y = r.y;
    return r.z > 0;
}

static void read_esp_state() {
    static int lastState = -1;
    std::ifstream f(ESP_STATE_FILE);
    if (!f.is_open()) return;
    int s = -1;
    f >> s;
    f.close();
    if (s != lastState) {
        g_esp_on.store(s == 1);
        lastState = s;
        LOGI("ESP durumu degisti: %s", s == 1 ? "ACIK" : "KAPALI");
    }
}

static void draw_esp() {
    void* cam = get_main_camera();
    void* em = get_entity_manager_instance();
    if (!cam || !em) return;
    void* dict = *(void**)((uint8_t*)em + FIELD_EntityManager_entities);
    if (!dict) return;
    void* arr = *(void**)((uint8_t*)dict + FIELD_Dict_entries);
    int32_t cnt = *(int32_t*)((uint8_t*)dict + FIELD_Dict_count);
    if (!arr || cnt <= 0 || cnt > 10000) return;

    Vec3 local = {0,0,0};
    bool lf = false;
    uint8_t* base = (uint8_t*)arr + ARRAY_FIRST_ELEMENT_OFFSET;

    for (int i = 0; i < cnt; i++) {
        uint8_t* e = base + i * ENTRY_SIZE;
        int32_t h = *(int32_t*)(e + 0x00);
        void* v = *(void**)(e + 0x10);
        if (h == -1 || !v) continue;
        if (*(void**)v != g_player_class) continue;
        typedef bool (*gb)(void*);
        gb isLocal = call_rva<gb>(RVA_Player_get_bLocalPlayer);
        if (isLocal && isLocal(v)) {
            typedef float (*gf)(void*);
            gf px = call_rva<gf>(RVA_Player_get_PosX_Smooth);
            gf py = call_rva<gf>(RVA_Player_get_PosY_Smooth);
            gf pz = call_rva<gf>(RVA_Player_get_PosZ_Smooth);
            local.x = px ? px(v) : 0; local.y = py ? py(v) : 0; local.z = pz ? pz(v) : 0;
            lf = true; break;
        }
    }

    for (int i = 0; i < cnt; i++) {
        uint8_t* e = base + i * ENTRY_SIZE;
        int32_t h = *(int32_t*)(e + 0x00);
        void* v = *(void**)(e + 0x10);
        if (h == -1 || !v) continue;
        if (*(void**)v != g_player_class) continue;

        typedef bool (*gb)(void*);
        gb isLocal = call_rva<gb>(RVA_Player_get_bLocalPlayer);
        if (isLocal && isLocal(v)) continue;
        gb isDead = call_rva<gb>(RVA_Player_get_IsDead);
        if (isDead && isDead(v)) continue;

        typedef float (*gf)(void*);
        gf px = call_rva<gf>(RVA_Player_get_PosX_Smooth);
        gf py = call_rva<gf>(RVA_Player_get_PosY_Smooth);
        gf pz = call_rva<gf>(RVA_Player_get_PosZ_Smooth);
        Vec3 p; 
        p.x = px ? px(v) : 0; p.y = py ? py(v) : 0; p.z = pz ? pz(v) : 0;

        gf ghp = call_rva<gf>(RVA_Player_get_Hp);
        gf gmh = call_rva<gf>(RVA_Player_get_MaxHp);
        float hp = ghp ? ghp(v) : 100;
        float mhp = gmh ? gmh(v) : 100;

        typedef void* (*gs)(void*);
        gs gn = call_rva<gs>(RVA_Player_get_Name);
        std::string nm = gn ? read_string(gn(v)) : "Player";

        Vec2 s;
        if (!w2s(cam, p, &s)) continue;
        if (s.x < 0 || s.x > g_screen_w || s.y < 0 || s.y > g_screen_h) continue;

        float dist = 0;
        if (lf) {
            float dx = p.x - local.x, dy = p.y - local.y, dz = p.z - local.z;
            dist = sqrtf(dx*dx + dy*dy + dz*dz);
        }

        Vec3 head = { p.x, p.y + 1.75f, p.z };
        Vec2 sHead;
        if (!w2s(cam, head, &sHead)) continue;

        float bh = s.y - sHead.y;
        if (bh < 5 || bh > 2000) continue;
        float bw = bh * 0.5f;
        float bx = sHead.x - bw * 0.5f;
        float by = sHead.y;

        gl_draw_rect(bx, by, bw, bh, 255, 0, 0, 255);

        if (!nm.empty()) {
            float tw = gl_text_width(nm.c_str(), 1.5f);
            gl_draw_text(s.x - tw*0.5f, by - 22, nm.c_str(), 1.5f, 255,255,255,255);
        }

        if (lf) {
            char d[32];
            snprintf(d, sizeof(d), "[%.0fm]", dist);
            float tw = gl_text_width(d, 1.2f);
            gl_draw_text(s.x - tw*0.5f, s.y + 4, d, 1.2f, 255,255,0,255);
        }

        if (mhp > 0) {
            float r = hp / mhp;
            if (r < 0) r = 0;
            if (r > 1) r = 1;
            gl_draw_filled_rect(bx-8, by, 4, bh, 0,0,0,200);
            gl_draw_filled_rect(bx-8, by + bh*(1-r), 4, bh*r, 0,255,0,255);
        }
    }
}

static void draw_status_marker() {
    if (g_esp_on.load()) {
        gl_draw_filled_rect(20, 20, 30, 30, 0, 255, 0, 255);
    }
}

static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    int c = g_egl_count.fetch_add(1) + 1;
    if (c == 1) LOGI(">>> eglSwapBuffers ILK CAGRI <<<");

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

        read_esp_state();
        draw_status_marker();

        if (g_esp_on.load()) {
            draw_esp();
        }
    }
    return g_orig_egl ? g_orig_egl(dpy, surf) : EGL_TRUE;
}

static int my_vkQueuePresentKHR(void* q, const void* info) {
    int c = g_vk_count.fetch_add(1) + 1;
    if (c == 1) LOGI(">>> vkQueuePresentKHR ILK CAGRI - VULKAN! <<<");
    return g_orig_vk ? g_orig_vk(q, info) : 0;
}

static void heartbeat_thread() {
    for (int i = 0; i < 1000; i++) {
        sleep(5);
        LOGI("[heartbeat] egl=%d vk=%d gl_ready=%d esp=%d",
             g_egl_count.load(), g_vk_count.load(),
             (int)g_gl_ready, (int)g_esp_on.load());
    }
}

static void install_hooks() {
    void* libegl = dlopen("libEGL.so", RTLD_NOW);
    if (libegl) {
        void* target = dlsym(libegl, "eglSwapBuffers");
        if (target) {
            LOGI("eglSwapBuffers addr: %p", target);
            DobbyHook(target, (void*)my_eglSwapBuffers, (void**)&g_orig_egl);
        }
    }

    void* libvk = dlopen("libvulkan.so", RTLD_NOW);
    if (libvk) {
        void* target = dlsym(libvk, "vkQueuePresentKHR");
        if (target) {
            LOGI("vkQueuePresentKHR addr: %p", target);
            DobbyHook(target, (void*)my_vkQueuePresentKHR, (void**)&g_orig_vk);
        }
    }
}

class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;

    void onLoad(Api* a, JNIEnv* e) override {
        api_ptr = a;
        env_ptr = e;
        LOGI("onLoad");
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!api_ptr || !env_ptr || !args || !args->nice_name) {
            if (api_ptr) api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        const char* p = env_ptr->GetStringUTFChars(args->nice_name, nullptr);
        bool ok = (p && strcmp(p, "com.tencent.rmos") == 0);
        if (p) env_ptr->ReleaseStringUTFChars(args->nice_name, p);
        if (!ok) {
            api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        g_is_target = true;
        LOGI("Rust Mobile tespit edildi");
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!g_is_target) return;

        std::thread([]() {
            int a = 0;
            while (a < 60 && find_lib_base("libil2cpp.so") == 0) {
                sleep(1);
                a++;
            }
            if (a >= 60) {
                LOGE("libil2cpp yuklenmedi");
                return;
            }
            if (!init_il2cpp_api()) {
                LOGE("init_il2cpp_api basarisiz oldu");
                return;
            }

            LOGI("il2cpp API basariyla yuklendi, hooklar kuruluyor...");
            install_hooks();

            g_player_class = find_class("WizardGames.Soc.Common.Entity", "PlayerEntity");
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("WizardGames.Soc.Share.Framework", "EntityManager");

            LOGI("P=%p C=%p EM=%p", g_player_class, g_camera_class, g_entity_manager_class);

            std::thread(heartbeat_thread).detach();
            LOGI("ESP tamamen hazir!");
        }).detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)