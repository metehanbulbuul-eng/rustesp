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

using namespace zygisk;

Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

// gl.cpp ve il2cpp.cpp fonksiyonları
bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
std::string read_string(void* il2cpp_str);

static bool g_is_target = false;
static bool g_hook_installed = false;

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
        // Her şeyi null check et, hata olursa modülü hemen kapat
        if (!api_ptr) {
            return;  // api yoksa hiçbir şey yapamayız
        }

        if (!env_ptr || !args || !args->nice_name) {
            LOGI("preAppSpecialize: null arg, closing module");
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
        // Target değilse hiçbir şey yapma
        if (!g_is_target) {
            return;
        }

        LOGI("postAppSpecialize: target app, spawning worker thread...");

        // Thread'i hemen başlat, ama içinde try-catch mantığı olsun
        std::thread worker([]() {
            // libil2cpp.so'nun yüklenmesini bekle
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

            LOGI("Siniflar: PlayerEntity=%p Camera=%p EntityManager=%p",
                 g_player_class, g_camera_class, g_entity_manager_class);

            g_hook_installed = true;
            LOGI("ESP hazır");
        });
        worker.detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)