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

Il2CppClass* g_player_class = nullptr;
Il2CppClass* g_camera_class = nullptr;
Il2CppClass* g_entity_manager_class = nullptr;

bool init_il2cpp_api();
Il2CppClass* find_class(const char* namespaze, const char* name);
std::string read_string(void* il2cpp_str);

static bool g_is_target = false;
static volatile int g_egl_count = 0;
static volatile int g_vk_count = 0;
static bool g_logged_once = false;

// === ELF symbol finder (libil2cpp için de kullanılıyor, buradan da çağıracağız) ===
typedef void* (*elf_find_t)(uintptr_t, const char*);
extern void* find_symbol_in_elf(uintptr_t base, const char* name);

// === lib base bul ===
static uintptr_t find_lib_base(const char* libname) {
    uintptr_t result = 0;
    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        const char* name = (const char*)((void**)data)[0];
        uintptr_t* out = (uintptr_t*)((void**)data)[1];
        if (info->dlpi_name && strstr(info->dlpi_name, name)) {
            *out = (uintptr_t)info->dlpi_addr;
            return 1;
        }
        return 0;
    }, (void*)((void*[]){ (void*)libname, &result }));
    return result;
}

// === ARM64 hook (basit) ===
static void* install_arm64_hook(void* target, void* hook_fn) {
    uint32_t* tcode = (uint32_t*)target;
    void* tramp = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) return nullptr;
    uint32_t* tramp_code = (uint32_t*)tramp;
    for (int i = 0; i < 4; i++) tramp_code[i] = tcode[i];
    tramp_code[4] = 0x58000050;
    tramp_code[5] = 0xD61F0200;
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

// === EGL hook ===
typedef EGLBoolean (*eglSwapBuffers_t)(EGLDisplay, EGLSurface);
static eglSwapBuffers_t g_orig_egl = nullptr;

static EGLBoolean my_eglSwapBuffers(EGLDisplay dpy, EGLSurface surf) {
    g_egl_count++;
    if (!g_logged_once && g_egl_count == 1) {
        LOGI(">>> eglSwapBuffers ÇAĞRILDI! Oyun OpenGL ES kullanıyor <<<");
    }
    return g_orig_egl(dpy, surf);
}

// === Vulkan vkQueuePresentKHR hook ===
typedef int (*vkQueuePresentKHR_t)(void* queue, const void* pPresentInfo);
static vkQueuePresentKHR_t g_orig_vk = nullptr;

static int my_vkQueuePresentKHR(void* queue, const void* info) {
    g_vk_count++;
    if (!g_logged_once && g_vk_count == 1) {
        LOGI(">>> vkQueuePresentKHR ÇAĞRILDI! Oyun Vulkan kullanıyor <<<");
        g_logged_once = true;
    }
    return g_orig_vk(queue, info);
}

// === libil2cpp base ===
static uintptr_t check_il2cpp_loaded() {
    return find_lib_base("libil2cpp.so");
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

// === Detect API hooks ===
static void install_detect_hooks() {
    // EGL
    void* libegl = dlopen("libEGL.so", RTLD_NOW);
    if (libegl) {
        void* target = dlsym(libegl, "eglSwapBuffers");
        if (target) {
            LOGI("eglSwapBuffers: %p", target);
            void* t = install_arm64_hook(target, (void*)my_eglSwapBuffers);
            if (t) {
                g_orig_egl = (eglSwapBuffers_t)t;
                LOGI("EGL hook OK");
            }
        }
    }
    // Vulkan
    void* libvk = dlopen("libvulkan.so", RTLD_NOW);
    if (libvk) {
        void* target = dlsym(libvk, "vkQueuePresentKHR");
        if (target) {
            LOGI("vkQueuePresentKHR: %p", target);
            void* t = install_arm64_hook(target, (void*)my_vkQueuePresentKHR);
            if (t) {
                g_orig_vk = (vkQueuePresentKHR_t)t;
                LOGI("Vulkan hook OK");
            }
        }
    }
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
            }

            g_player_class = find_class("WizardGames.Soc.Common.Entity", "PlayerEntity");
            g_camera_class = find_class("UnityEngine", "Camera");
            g_entity_manager_class = find_class("WizardGames.Soc.Share.Framework", "EntityManager");

            LOGI("Siniflar: P=%p C=%p EM=%p",
                 g_player_class, g_camera_class, g_entity_manager_class);

            // API detection hook'larını kur
            install_detect_hooks();

            LOGI("ESP hazir! Hook'lar kuruldu, hangi API kullaniliyor bekleniyor...");
        });
        worker.detach();
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)