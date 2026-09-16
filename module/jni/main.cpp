#include <sys/types.h>
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
void RunESPloop();

// Global değişkenler
Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

static bool g_is_target = false;
static bool g_gl_ready = false;
static std::atomic<bool> g_esp_on{true}; // Test için açık başlasın
static std::atomic<int> g_egl_count{0};
static int g_screen_w = 1080, g_screen_h = 2400;

#define ESP_STATE_FILE "/data/local/tmp/esp_state.txt"

// EGL hook için pointer (Dobby olmadığı için şimdilik sadece log basıyoruz)
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t g_orig_egl = nullptr;

// Güvenli EGL SwapBuffers simülasyonu / callback tetikleyicisi
static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    int c = g_egl_count.fetch_add(1) + 1;
    if (c == 1) LOGI(">>> eglSwapBuffers ILK CAGRI (Grafik Devrede) <<<");

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

        // Test amaçlı ekrana küçük bir kutu çizelim (Modülün çizim yapıp yapmadığını görmek için)
        gl_draw_filled_rect(50, 50, 100, 100, 0, 255, 0, 255);
    }

    // Orijinal fonksiyon olmadığı için direkt EGL_TRUE dönüyoruz (oyun çökmesin diye)
    return EGL_TRUE; 
}

class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;

    void onLoad(Api* a, JNIEnv* e) override {
        api_ptr = a;
        env_ptr = e;
        LOGI("RustESP onLoad basariyla calisti");
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
        LOGI("Rust Mobile (com.tencent.rmos) tespit edildi!");
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!g_is_target) return;

        // Oyun açılışını bloklamamak için her şeyi arka plan iş parçacığına (thread) alıyoruz
        std::thread([]() {
            LOGI("Arka plan baslatildi, libil2cpp.so bekleniyor...");
            int a = 0;
            
            // libil2cpp.so'nun belleğe yüklenmesi için max 60 saniye bekleyelim
            uintptr_t il2cpp_base = 0;
            while (a < 60) {
                // il2cpp.cpp içerisindeki fonksiyonu tetiklemek için base kontrolü
                // Basit bir dlopen kontrolü yapalım
                void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
                if (handle) {
                    dlclose(handle);
                    break;
                }
                sleep(1);
                a++;
            }

            if (a >= 60) {
                LOGE("Zaman asimi: libil2cpp.so yuklenemedi!");
                return;
            }

            LOGI("libil2cpp.so algilandi, API baslatiliyor...");
            if (!init_il2cpp_api()) {
                LOGE("init_il2cpp_api basarisiz!");
                return;
            }

            LOGI("Il2cpp API basariyla hazir!");
            
            // Sınıf bulma denemeleri (Çökme yapmaması için kontrollü)
            g_player_class = find_class("WizardGames.Soc.Common.Entity", "PlayerEntity");
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("WizardGames.Soc.Share.Framework", "EntityManager");

            LOGI("Bulunan Siniflar -> Player: %p | Camera: %p | EntityManager: %p", 
                 g_player_class, g_camera_class, g_entity_manager_class);

            // Döngü test thread'i
            while (true) {
                RunESPloop();
                sleep(1);
            }

        }).detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)