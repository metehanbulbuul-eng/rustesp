#include "esp.h"
#include "offsets.h"
#include "zygisk.hpp"
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <mutex>

using namespace zygisk;

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

// Inline hook için ARM64 trambolin
static uint32_t g_saved_instructions[4] = {0};

// === ARM64 inline hook (basit versiyon) ===
// Bu, ilk 16 byte'ı trambolin'e taşır ve orijinal koda atlar
// Gerçek projede Dobby veya ShadowHook kullanılmalı ama bu da çalışır

static void* g_trampoline = nullptr;

// === Oyuncu listesini oku ===
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
void* get_main_camera() {
    typedef void* (*get_main_t)();
    get_main_t fn = call_rva<get_main_t>(RVA_Camera_get_main);
    return fn();
}

// === Dünya -> Ekran ===
bool world_to_screen(void* camera, Vec3 world, Vec2* out) {
    if (!camera || !out) return false;
    typedef Vec3 (*w2s_t)(void*, Vec3, int);
    // WorldToScreenPoint(Vector3 position, MonoOrStereoscopicEye eye)
    // eye = 0 (Mono)
    w2s_t fn = (w2s_t)call_rva<void*>(RVA_Camera_WorldToScreenPoint);
    // Not: Fonksiyon aslında bir struct döndürür (Vector3 - 12 byte),
    // ARM64'te bu x0'da döner. Basitçe:
    // Bu kısmı daha sonra refine edeceğiz.
    return false;  // Placeholder
}

// === EntityManager'dan oyuncuları çek ===
std::vector<PlayerSnapshot> collect_players() {
    std::vector<PlayerSnapshot> result;

    if (!api.domain_get) return result;

    // EntityManager.Instance
    typedef void* (*get_instance_t)();
    get_instance_t get_inst = call_rva<get_instance_t>(RVA_EntityManager_get_Instance);
    void* entity_mgr = get_inst();
    if (!entity_mgr) return result;

    // EntityManager.entities (Dictionary<long, EntityBase>)
    // Field offset: 0x18
    void* dict = *(void**)((uint8_t*)entity_mgr + FIELD_EntityManager_entities);
    if (!dict) return result;

    // Dictionary iç yapısı:
    // _buckets: 0x10
    // _entries: 0x18
    // _count: 0x20
    // (IL2CPP'de obje başlığı 16 byte)
    void* entries_array = *(void**)((uint8_t*)dict + FIELD_Dict_entries);
    int count = *(int*)((uint8_t*)dict + FIELD_Dict_count);
    if (!entries_array || count <= 0 || count > 10000) return result;

    // Entry dizisi üzerinden dön
    // Array data offset: 0x20 (IL2CPP array header)
    uint8_t* entry_base = (uint8_t*)entries_array + ARRAY_FIRST_ELEMENT_OFFSET;

    for (int i = 0; i < count; i++) {
        uint8_t* entry = entry_base + (i * ENTRY_SIZE);
        int32_t hashCode = *(int32_t*)(entry + 0x00);
        int32_t next     = *(int32_t*)(entry + 0x04);
        int64_t key      = *(int64_t*)(entry + 0x08);
        void*   value    = *(void**)(entry + 0x10);

        // hashCode == -1 ise bu slot boş
        if (hashCode == -1 || !value) continue;

        // value bir EntityBase*. Sınıfını kontrol et.
        // İlk 8 byte: klass pointer
        void* klass = *(void**)value;

        // Sınıf adını al
        if (!api.class_get_name) continue;
        const char* cls_name = api.class_get_name(klass);
        if (!cls_name) continue;

        // PlayerEntity mi?
        if (strcmp(cls_name, "PlayerEntity") != 0) continue;

        // === Oyuncu bilgilerini oku ===
        PlayerSnapshot snap;
        memset(&snap, 0, sizeof(snap));

        // is_local: get_bLocalPlayer
        typedef bool (*get_bool_t)(void*);
        get_bool_t get_blocal = call_rva<get_bool_t>(RVA_Player_get_bLocalPlayer);
        snap.is_local = get_blocal(value);

        // is_dead: get_IsDead
        get_bool_t get_isdead = call_rva<get_bool_t>(RVA_Player_get_IsDead);
        snap.is_dead = get_isdead(value);

        // Position: get_PosX_Smooth / PosY_Smooth / PosZ_Smooth
        typedef float (*get_float_t)(void*);
        get_float_t get_px = call_rva<get_float_t>(RVA_Player_get_PosX_Smooth);
        get_float_t get_py = call_rva<get_float_t>(RVA_Player_get_PosY_Smooth);
        get_float_t get_pz = call_rva<get_float_t>(RVA_Player_get_PosZ_Smooth);
        snap.pos.x = get_px(value);
        snap.pos.y = get_py(value);
        snap.pos.z = get_pz(value);

        // HP: get_Hp / MaxHp
        get_float_t get_hp    = call_rva<get_float_t>(RVA_Player_get_Hp);
        get_float_t get_maxhp = call_rva<get_float_t>(RVA_Player_get_MaxHp);
        snap.hp     = get_hp(value);
        snap.max_hp = get_maxhp(value);

        // Team: get_TeamId
        typedef int64_t (*get_i64_t)(void*);
        get_i64_t get_team = call_rva<get_i64_t>(RVA_Player_get_TeamId);
        snap.team_id = (int)get_team(value);

        // RoleId
        get_i64_t get_role = call_rva<get_i64_t>(RVA_Player_get_RoleId);
        snap.role_id = get_role(value);

        // İsim: get_Name
        typedef void* (*get_str_t)(void*);
        get_str_t get_name = call_rva<get_str_t>(RVA_Player_get_Name);
        void* name_obj = get_name(value);
        snap.name = read_string(name_obj);

        result.push_back(snap);
    }

    return result;
}

// === ESP çizimi ===
void draw_esp() {
    if (!g_esp_enabled) return;

    // Kamerayı al
    void* cam = get_main_camera();
    if (!cam) return;

    // Oyuncuları topla
    auto players = collect_players();
    if (players.empty()) return;

    // Yerel oyuncuyu bul
    Vec3 local_pos = {0, 0, 0};
    bool local_found = false;
    for (auto& p : players) {
        if (p.is_local) {
            local_pos = p.pos;
            local_found = true;
            break;
        }
    }

    // Her oyuncu için ESP çiz
    for (auto& p : players) {
        if (p.is_local) continue;  // Yerel oyuncuyu çizme
        if (p.is_dead) continue;   // Ölüleri çizme

        // Dünya -> Ekran
        Vec2 screen;
        // Geçici olarak WorldToScreenPoint'i manuel çağıralım
        // ARM64'te Vector3 döndüren fonksiyon x0'a 12 byte'lık yapıyı yazar
        typedef void (*w2s_t)(void*, Vec3*, Vec2*);
        // Bu kısmı ileride test edip düzelteceğiz
        // Şimdilik skiple

        // Not: WorldToScreenPoint test edilene kadar ESP çizilmeyecek
        // Aşağıdaki kodu W2S çalışınca açacağız:
        /*
        float dx = screen.x - screen.x;
        float dy = screen.y - screen.y;
        float dist = sqrtf(dx*dx + dy*dy);

        // Kutu boyutu
        float box_h = 8000.0f / (screen.y + 1.0f);
        float box_w = box_h * 0.5f;

        // Kutu
        gl_draw_rect(screen.x - box_w/2, screen.y - box_h,
                     box_w, box_h, 255, 0, 0, 255);

        // İsim
        gl_draw_text(screen.x - gl_text_width(p.name.c_str(), 1.5f)/2,
                     screen.y - box_h - 20,
                     p.name.c_str(), 1.5f, 255, 255, 255, 255);

        // HP barı
        if (p.max_hp > 0) {
            float hp_ratio = p.hp / p.max_hp;
            gl_draw_filled_rect(screen.x - box_w/2 - 6, screen.y - box_h,
                                 4, box_h, 0, 0, 0, 200);
            gl_draw_filled_rect(screen.x - box_w/2 - 6,
                                 screen.y - box_h + box_h * (1 - hp_ratio),
                                 4, box_h * hp_ratio, 0, 255, 0, 255);
        }

        // Mesafe
        if (local_found) {
            float ddx = p.pos.x - local_pos.x;
            float ddy = p.pos.y - local_pos.y;
            float ddz = p.pos.z - local_pos.z;
            float dist = sqrtf(ddx*ddx + ddy*ddy + ddz*ddz);
            char dist_str[32];
            snprintf(dist_str, sizeof(dist_str), "[%.0fm]", dist);
            gl_draw_text(screen.x - gl_text_width(dist_str, 1.2f)/2,
                         screen.y + 5, dist_str, 1.2f, 255, 255, 0, 255);
        }
        */
    }
}

// === eglSwapBuffers hook ===
static EGLBoolean hook_eglSwapBuffers(EGLDisplay dpy, EGLSurface surface) {
    // İlk çağrıda OpenGL'i başlat
    if (!g_initialized) {
        // Ekran boyutunu EGL'den al
        EGLint w = 0, h = 0;
        eglQuerySurface(dpy, surface, EGL_WIDTH, &w);
        eglQuerySurface(dpy, surface, EGL_HEIGHT, &h);
        gl_set_screen_size(w, h);

        if (gl_init()) {
            g_initialized = true;
            LOGI("ESP başlatıldı. Ekran: %dx%d", w, h);
        }
    }

    // Önce ESP çiz (oyunun kendi çiziminden SONRA olması için
    // bu hook'un swap öncesi çağrıldığını varsayıyoruz)
    if (g_initialized) {
        glViewport(0, 0, 1080, 2400);  // Ekran boyutunu kullan
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // Matrisleri sıfırla (2D mod)
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();

        draw_esp();
    }

    // Orijinal fonksiyonu çağır
    return g_orig_eglSwapBuffers(dpy, surface);
}

// === eglSwapBuffers'ı hook'la ===
// Basit yaklaşım: PLT/GOT overwrite yerine, doğrudan fonksiyon adresini değiştir
// (Bu, libEGL'nin kendi GOT'unu değiştirir; her zaman çalışmaz ama deneyelim)

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

    // Not: Gerçek inline hook için Dobby veya ShadowHook gerekli.
    // Şimdilik sadece orijinal fonksiyonu kaydediyoruz.
    // İleride burada hook kuracağız.

    return true;
}

// === Zygisk modülü ===
class RustESPModule : public ModuleBase {
public:
    void onLoad() override {
        LOGI("RustESP Zygisk modülü yüklendi");
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        // DenyList'teysek hiçbir şey yapma
        if (isDenylisted()) {
            setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        // Paket adını kontrol et
        const char* pkg = args->nice_name;
        if (pkg && strcmp(pkg, "com.tencent.rmos") != 0) {
            setOption(DLCLOSE_MODULE_LIBRARY);
            return;
        }

        LOGI("Rust Mobile tespit edildi, ESP hazırlanıyor...");
    }

    void postAppSpecialize(const AppSpecializeArgs* args) override {
        if (isDenylisted()) return;

        // Arka planda IL2CPP API'sini başlat
        // Oyun libil2cpp.so'yu yükleyene kadar beklememiz lazım
        // Bunun için bir thread başlatalım
        std::thread([]() {
            // libil2cpp.so'nun yüklenmesini bekle
            int attempts = 0;
            while (attempts < 60) {  // 30 saniye bekle
                void* h = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
                if (h) {
                    dlclose(h);
                    break;
                }
                sleep(1);
                attempts++;
            }

            if (attempts >= 60) {
                LOGE("libil2cpp.so 30 saniye içinde yüklenmedi");
                return;
            }

            LOGI("libil2cpp.so yüklendi, ESP başlatılıyor...");

            // IL2CPP API'sini başlat
            if (!init_il2cpp_api()) {
                LOGE("IL2CPP API başlatılamadı");
                return;
            }

            // Sınıfları bul
            g_player_class = find_class("", "PlayerEntity");
            if (!g_player_class) {
                g_player_class = find_class("Soc", "PlayerEntity");
            }
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("", "EntityManager");

            LOGI("Sınıflar: PlayerEntity=%p Camera=%p EntityManager=%p",
                 g_player_class, g_camera_class, g_entity_manager_class);

            // EGL hook'u kur
            if (!install_egl_hook()) {
                LOGE("EGL hook kurulamadı");
                return;
            }

            LOGI("ESP hazır, oyun başladığında çizim yapılacak");
        }).detach();
    }
};

// === Zygisk API kaydı ===
REGISTER_ZYGISK_MODULE(RustESPModule)