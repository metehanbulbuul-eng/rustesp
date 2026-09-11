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
#include <math.h>
#include <sys/mman.h>

using namespace zygisk;

// gl.cpp fonksiyonları
bool  gl_init();
void  gl_set_screen_size(int w, int h);
void  gl_draw_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_text(float x, float y, const char* text, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
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
static int g_screen_w = 0, g_screen_h = 0;

// EGL hook
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t g_orig_eglSwapBuffers = nullptr;

// === ARM64 naive inline hook ===
static void* install_arm64_hook(void* target, void* hook_fn) {
    uint32_t* tcode = (uint32_t*)target;
    void* tramp = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
    uint32_t* tramp_code = (uint32_t*)tramp;
    for (int i = 0; i < 4; i++) tramp_code[i] = tcode[i];
    tramp_code[4] = 0x58000050; // ldr x16, #8
    tramp_code[5] = 0xD61F0200; // br x16
    *(uint64_t*)&tramp_code[6] = (uint64_t)target + 16;

    uintptr_t page = (uintptr_t)target & ~0xFFFULL;
    mprotect((void*)page, 4096, PROT_READ | PROT_WRITE | PROT_EXEC);
    tcode[0] = 0x58000050;
    tcode[1] = 0xD61F0200;
    *(uint64_t*)&tcode[2] = (uint64_t)hook_fn;
    mprotect((void*)page, 4096, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)target, (char*)target + 16);
    return tramp;
}

// === libil2cpp.so base ===
static uintptr_t check_il2cpp_loaded() {
    uintptr_t result = 0;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
            *(uintptr_t*)data = (uintptr_t)info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &result);
    return result;
}

// === EntityManager.Instance ===
static void* get_entity_manager_instance() {
    typedef void* (*get_instance_t)();
    get_instance_t fn = call_rva<get_instance_t>(RVA_EntityManager_get_Instance);
    return fn();
}

// === Kamera ===
static void* get_main_camera() {
    typedef void* (*get_main_t)();
    get_main_t fn = call_rva<get_main_t>(RVA_Camera_get_main);
    return fn();
}

// === World to Screen ===
struct W2SResult { float x, y, z; };
static bool world_to_screen(void* camera, Vec3 world, Vec2* out_screen) {
    if (!camera || !out_screen) return false;
    typedef W2SResult (*w2s_t)(void*, Vec3);
    w2s_t fn = (w2s_t)call_rva<void*>(RVA_Camera_WorldToScreenPoint);
    W2SResult r = fn(camera, world);
    out_screen->x = r.x;
    out_screen->y = r.y;
    return r.z > 0;
}

// === ESP çizim ===
static void draw_esp() {
    void* camera = get_main_camera();
    if (!camera) return;
    void* em = get_entity_manager_instance();
    if (!em) return;

    void* dict = *(void**)((uint8_t*)em + 0x18);
    if (!dict) return;
    void* entries_array = *(void**)((uint8_t*)dict + 0x18);
    int32_t count = *(int32_t*)((uint8_t*)dict + 0x20);
    if (!entries_array || count <= 0 || count > 10000) return;

    Vec3 local_pos = {0, 0, 0};
    bool local_found = false;
    uint8_t* entry_base = (uint8_t*)entries_array + 0x20;

    // Önce yerel oyuncu
    for (int i = 0; i < count; i++) {
        uint8_t* entry = entry_base + i * 0x18;
        int32_t hash = *(int32_t*)(entry + 0x00);
        void* value = *(void**)(entry + 0x10);
        if (hash == -1 || !value) continue;
        void* klass = *(void**)value;
        if (klass != g_player_class) continue;
        typedef bool (*get_bool_t)(void*);
        get_bool_t get_bLocal = call_rva<get_bool_t>(RVA_Player_get_bLocalPlayer);
        if (get_bLocal(value)) {
            typedef float (*get_f_t)(void*);
            get_f_t px = call_rva<get_f_t>(RVA_Player_get_PosX_Smooth);
            get_f_t py = call_rva<get_f_t>(RVA_Player_get_PosY_Smooth);
            get_f_t pz = call_rva<get_f_t>(RVA_Player_get_PosZ_Smooth);
            local_pos.x = px(value);
            local_pos.y = py(value);
            local_pos.z = pz(value);
            local_found = true;
            break;
        }
    }

    // Diğer oyuncular
    for (int i = 0; i < count; i++) {
        uint8_t* entry = entry_base + i * 0x18;
        int32_t hash = *(int32_t*)(entry + 0x00);
        void* value = *(void**)(entry + 0x10);
        if (hash == -1 || !value) continue;
        void* klass = *(void**)value;
        if (klass != g_player_class) continue;

        typedef bool (*get_bool_t)(void*);
        get_bool_t get_bLocal = call_rva<get_bool_t>(RVA_Player_get_bLocalPlayer);
        if (get_bLocal(value)) continue;
        get_bool_t get_isDead = call_rva<get_bool_t>(RVA_Player_get_IsDead);
        if (get_isDead(value)) continue;

        typedef float (*get_f_t)(void*);
        get_f_t px = call_rva<get_f_t>(RVA_Player_get_PosX_Smooth);
        get_f_t py = call_rva<get_f_t>(RVA_Player_get_PosY_Smooth);
        get_f_t pz = call_rva<get_f_t>(RVA_Player_get_PosZ_Smooth);
        Vec3 pos;
        pos.x = px(value);
        pos.y = py(value);
        pos.z = pz(value);

        get_f_t get_hp = call_rva<get_f_t>(RVA_Player_get_Hp);
        get_f_t get_maxhp = call_rva<get_f_t>(RVA_Player_get_MaxHp);
        float hp = get_hp(value);
        float max_hp = get_maxhp(value);

        typedef void* (*get_str_t)(void*);
        get_str_t get_name = call_rva<get_str_t>(RVA_Player_get_Name);
        void* name_obj = get_name(value);
        std::string name = read_string(name_obj);

        Vec2 screen;
        if (!world_to_screen(camera, pos, &screen)) continue;
        if (screen.x < 0 || screen.x > g_screen_w) continue;
        if (screen.y < 0 || screen.y > g_screen_h) continue;

        float dist = 0;
        if (local_found) {
            float dx = pos.x - local_pos.x;
            float dy = pos.y - local_pos.y;
            float dz = pos.z - local_pos.z;
            dist = sqrtf(dx*dx + dy*dy + dz*dz);
        }

        float box_h = 8000.0f / (dist + 1.0f);
        float box_w = box_h * 0.5f;
        float bx = screen.x - box_w * 0.5f;
        float by = screen.y - box_h;

        // Kırmızı kutu
        gl_draw_rect(bx, by, box_w, box_h, 255, 0, 0, 255);

        // Beyaz isim
        if (!name.empty()) {
            float tw = gl_text_width(name.c_str(), 1.5f);
            gl_draw_text(screen.x - tw * 0.5f, by - 22, name.c_str(), 1.5f, 255, 255, 255, 255);
        }

        // Sarı mesafe
        if (local_found) {
            char dbuf[32];
            snprintf(dbuf, sizeof(dbuf), "[%.0fm]", dist);
            float tw = gl_text_width(dbuf, 1.2f);
            gl_draw_text(screen.x - tw * 0.5f, screen.y + 4, dbuf, 1.2f, 255, 255, 0, 255);
        }

        // Can barı
        if (max_hp > 0) {
            float ratio = hp / max_hp;
            if (ratio < 0) ratio = 0;
            if (ratio > 1) ratio = 1;
            gl_draw_filled_rect(bx - 8, by, 4, box_h, 0, 0, 0, 200);
            gl_draw_filled_rect(bx - 8, by + box_h * (1 - ratio), 4, box_h * ratio, 0, 255, 0, 255);
        }
    }
}

// === EGL hook ===
static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    static bool thread_attached = false;
    if (!thread_attached) {
        Il2CppDomain* domain = (Il2CppDomain*)api.domain_get();
        if (domain && api.thread_attach) {
            api.thread_attach(domain);
            thread_attached = true;
        }
    }

    if (!g_gl_ready) {
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
        if (w > 0 && h > 0) {
            g_screen_w = w;
            g_screen_h = h;
            if (gl_init()) {
                g_gl_ready = true;
                LOGI("GL init: %dx%d", w, h);
            }
        }
    }

    if (g_gl_ready) {
        glViewport(0, 0, g_screen_w, g_screen_h);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_esp();
    }
    return g_orig_eglSwapBuffers(dpy, surf);
}

static void install_egl_hook() {
    void* libegl = dlopen("libEGL.so", RTLD_NOW);
    if (!libegl) { LOGE("libEGL yuklenemedi"); return; }
    void* target = dlsym(libegl, "eglSwapBuffers");
    if (!target) { LOGE("eglSwapBuffers yok"); return; }
    LOGI("eglSwapBuffers: %p", target);
    void* tramp = install_arm64_hook(target, (void*)my_eglSwapBuffers);
    if (!tramp) { LOGE("Hook kurulamadi"); return; }
    g_orig_eglSwapBuffers = (eglSwapBuffers_t)tramp;
    LOGI("EGL hook OK");
}

// === Zygisk modülü ===
class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;

    void onLoad(Api* api_, JNIEnv* env_) override {
        api_ptr = api_;
        env_ptr = env_;
        LOGI("onLoad called");
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!api_ptr) return;
        if (!env_ptr || !args || !args->nice_name) {
            api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        const char* pkg = env_ptr->GetStringUTFChars(args->nice_name, nullptr);
        LOGI("preAppSpecialize: pkg=%s", pkg ? pkg : "(null)");
        bool is_target = (pkg && strcmp(pkg, "com.tencent.rmos") == 0);
        if (pkg) env_ptr->ReleaseStringUTFChars(args->nice_name, pkg);
        if (!is_target) {
            api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        g_is_target = true;
        LOGI("Rust Mobile tespit edildi!");
    }

    void postAppSpecialize(const AppSpecializeArgs* args) override {
        if (!g_is_target) return;
        LOGI("postAppSpecialize: worker thread");
        std::thread worker([]() {
            int attempts = 0;
            while (attempts < 60) {
                if (check_il2cpp_loaded() != 0) break;
                sleep(1);
                attempts++;
            }
            if (attempts >= 60) { LOGE("libil2cpp yuklenmedi"); return; }

            if (!init_il2cpp_api()) { LOGE("API basarisiz"); return; }

            LOGI("10sn bekleniyor...");
            sleep(10);

            Il2CppDomain* domain = (Il2CppDomain*)api.domain_get();
            if (domain && api.thread_attach) {
                api.thread_attach(domain);
                LOGI("Thread attach edildi");
            }

            g_player_class = find_class("WizardGames.Soc.Common.Entity", "PlayerEntity");
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("WizardGames.Soc.Share.Framework", "EntityManager");

            LOGI("Siniflar: P=%p C=%p EM=%p",
                 g_player_class, g_camera_class, g_entity_manager_class);

            if (!g_player_class || !g_camera_class || !g_entity_manager_class) {
                LOGE("Siniflar eksik!");
                return;
            }

            install_egl_hook();
            LOGI("ESP hazir!");
        });
        worker.detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)