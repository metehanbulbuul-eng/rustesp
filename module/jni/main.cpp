#include "esp.h"
#include "offsets.h"
#include "zygisk.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include <mutex>

using namespace zygisk;

// === Global sınıf pointer'ları (esp.h'de extern) ===
Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

// === gl.cpp fonksiyonları ===
bool  gl_init();
void  gl_set_screen_size(int w, int h);
void  gl_draw_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_line(float x1, float y1, float x2, float y2, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void  gl_draw_text(float x, float y, const char* text, float scale, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
float gl_text_width(const char* text, float scale);

// === il2cpp.cpp fonksiyonları ===
bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
std::string read_string(void* il2cpp_str);

// === Global durum ===
static bool g_initialized = false;
static bool g_esp_enabled = true;

// === EGL hook ===
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay dpy, EGLSurface surface);
static eglSwapBuffers_t g_orig_eglSwapBuffers = nullptr;

// === Oyuncu snapshot ===
struct PlayerSnapshot {
    Vec3 pos;
    std::string name;
    float hp;
    float max_hp;
    int team_id;
    bool is_local;
    bool is_dead;
    uint64_t role_id;
};

// === Kamerayı al ===
static void* get_main_camera() {
    typedef void* (*get_main_t)();
    get_main_t fn = call_rva<get_main_t>(RVA_Camera_get_main);
    return fn();
}

// === EntityManager'dan oyuncuları çek ===
static std::vector<PlayerSnapshot> collect_players() {
    std::vector<PlayerSnapshot> result;
    if (!api.domain_get) return result;

    typedef void* (*get_instance_t)();
    get_instance_t get_inst = call_rva<get_instance_t>(RVA_EntityManager_get_Instance);
    void* entity_mgr = get_inst();
    if (!entity_mgr) return result;

    void* dict = *(void**)((uint8_t*)entity_mgr + FIELD_EntityManager_entities);
    if (!dict) return result;

    void* entries_array = *(void**)((uint8_t*)dict + FIELD_Dict_entries);
    int count = *(int*)((uint8_t*)dict + FIELD_Dict_count);
    if (!entries_array || count <= 0 || count > 10000) return result;

    uint8_t* entry_base = (uint8_t*)entries_array + ARRAY_FIRST_ELEMENT_OFFSET;

    for (int i = 0; i < count; i++) {
        uint8_t* entry = entry_base + (i * ENTRY_SIZE);
        int32_t hashCode = *(int32_t*)(entry + 0x00);
        void*   value    = *(void**)(entry + 0x10);

        if (hashCode == -1 || !value) continue;

        void* klass = *(void**)value;
        if (!api.class_get_name) continue;
        const char* cls_name = api.class_get_name(klass);
        if (!cls_name) continue;

        if (strcmp(cls_name, "PlayerEntity") != 0) continue;

        PlayerSnapshot snap;
        memset(&snap, 0, sizeof(snap));

        typedef bool (*get_bool_t)(void*);
        get_bool_t get_blocal = call_rva<get_bool_t>(RVA_Player_get_bLocalPlayer);
        snap.is_local = get_blocal(value);

        get_bool_t get_isdead = call_rva<get_bool_t>(RVA_Player_get_IsDead);
        snap.is_dead = get_isdead(value);

        typedef float (*get_float_t)(void*);
        get_float_t get_px = call_rva<get_float_t>(RVA_Player_get_PosX_Smooth);
        get_float_t get_py = call_rva<get_float_t>(RVA_Player_get_PosY_Smooth);
        get_float_t get_pz = call_rva<get_float_t>(RVA_Player_get_PosZ_Smooth);
        snap.pos.x = get_px(value);
        snap.pos.y = get_py(value);
        snap.pos.z = get_pz(value);

        get_float_t get_hp    = call_rva<get_float_t>(RVA_Player_get_Hp);
        get_float_t get_maxhp = call_rva<get_float_t>(RVA_Player_get_MaxHp);
        snap.hp     = get_hp(value);
        snap.max_hp = get_maxhp(value);

        typedef int64_t (*get_i64_t)(void*);
        get_i64_t get_team = call_rva<get_i64_t>(RVA_Player_get_TeamId);
        snap.team_id = (int)get_team(value);

        get_i64_t get_role = call_rva<get_i64_t>(RVA_Player_get_RoleId);
        snap.role_id = get_role(value);

        typedef void* (*get_str_t)(void*);
        get_str_t get_name = call_rva<get_str_t>(RVA_Player_get_Name);
        void* name_obj = get_name(value);
        snap.name = read_string(name_obj);

        result.push_back(snap);
    }

    return result;
}

// === ESP çizimi ===
static void draw_esp() {
    if (!g_esp_enabled) return;

    void* cam = get_main_camera();
    if (!cam) return;

    auto players = collect_players();
    if (players.empty()) return;

    Vec3 local_pos = {0, 0, 0};
    bool local_found = false;
    for (auto& p : players) {
        if (p.is_local) {
            local_pos = p.pos;
            local_found = true;
            break;
        }
    }
    (void)local_found;

    // Her oyuncu için ileride çizim yapılacak.
    // Şimdilik sadece loga düşürelim, W2S testi için.
    static int log_counter = 0;
    if (log_counter++ % 300 == 0) {
        LOGI("Oyuncu sayısı: %zu", players.size());
    }
}

// === eglSwapBuffers hook ===
static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    if (!g_initialized) {
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        gl_set_screen_size(w, h);

        if (gl_init()) {
            g_initialized = true;
            LOGI("ESP başlatıldı. Ekran: %dx%d", w, h);
        }
    }

    if (g_initialized) {
        glViewport(0, 0, 1080, 2400);
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        draw_esp();
    }

    return g_orig_eglSwapBuffers(dpy, surface);
}

static bool install_egl_hook() {
    void* libegl = dlopen("libEGL.so", RTLD_NOW);
    if (!libegl) {
        LOGE("libEGL.so yüklenemedi");
        return false;
    }
    void* target = dlsym(libegl, "eglSwapBuffers");
    if (!target) {
        LOGE("eglSwapBuffers bulunamadı");
        return false;
    }
    g_orig_eglSwapBuffers = (eglSwapBuffers_t)target;
    LOGI("eglSwapBuffers: %p", target);
    // Not: Gerçek inline hook ileride eklenecek (Dobby/ShadowHook).
    return true;
}

// === Zygisk modülü ===
class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;

    void onLoad(Api* api_, JNIEnv* env_) override {
        api_ptr = api_;
        env_ptr = env_;
        LOGI("RustESP Zygisk modülü yüklendi");
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!env_ptr) return;

        const char* pkg = env_ptr->GetStringUTFChars(args->nice_name, nullptr);
        bool is_target = (pkg && strcmp(pkg, "com.tencent.rmos") == 0);
        if (pkg) env_ptr->ReleaseStringUTFChars(args->nice_name, pkg);

        if (!is_target) {
            api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGI("Rust Mobile tespit edildi, ESP hazırlanıyor...");
    }

    void postAppSpecialize(const AppSpecializeArgs* args) override {
        std::thread([]() {
            int attempts = 0;
            while (attempts < 60) {
                void* h = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
                if (h) {
                    dlclose(h);
                    break;
                }
                sleep(1);
                attempts++;
            }

            if (attempts >= 60) {
                LOGE("libil2cpp.so 60 saniye içinde yüklenmedi");
                return;
            }

            LOGI("libil2cpp.so yüklendi, ESP başlatılıyor...");

            if (!init_il2cpp_api()) {
                LOGE("IL2CPP API başlatılamadı");
                return;
            }

            g_player_class = find_class("", "PlayerEntity");
            if (!g_player_class) {
                g_player_class = find_class("Soc", "PlayerEntity");
            }
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("", "EntityManager");

            LOGI("Sınıflar: PlayerEntity=%p Camera=%p EntityManager=%p",
                 g_player_class, g_camera_class, g_entity_manager_class);

            if (!install_egl_hook()) {
                LOGE("EGL hook kurulamadı");
                return;
            }

            LOGI("ESP hazır, oyun başladığında çizim yapılacak");
        }).detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)