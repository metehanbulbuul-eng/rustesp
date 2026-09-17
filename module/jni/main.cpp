#include <sys/types.h>
#include "esp.h"
#include "offsets.h"
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

// gl.cpp fonksiyonları
bool gl_init();
void gl_set_screen_size(int w, int h);
void gl_draw_filled_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, uint8_t a);

// il2cpp.cpp fonksiyonları
bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
void RunESPloop();

Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

// Oyun açıldığında kütüphane otomatik yüklendiğinde çalışacak ana fonksiyon
void __attribute__((constructor)) init() {
    LOGI("RustESP Harici Yükleyici Başlatıldı!");

    std::thread([]() {
        int a = 0;
        while (a < 60) {
            void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD);
            if (handle) {
                dlclose(handle);
                break;
            }
            sleep(1);
            a++;
        }

        if (a >= 60) {
            LOGE("libil2cpp.so zaman aşımı!");
            return;
        }

        if (!init_il2cpp_api()) {
            LOGE("init_il2cpp_api başarısız!");
            return;
        }

        LOGI("İl2cpp API Hazır, döngü başlatılıyor...");
        while (true) {
            RunESPloop();
            sleep(1);
        }
    }).detach();
}