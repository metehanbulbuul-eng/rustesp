#include <sys/types.h>
#include "zygisk.hpp"
#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <cstring>

#define LOG_TAG "RustESP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using namespace zygisk;

class RustESPModule : public ModuleBase {
public:
    Api* api_ptr = nullptr;
    JNIEnv* env_ptr = nullptr;
    bool is_target = false;

    void onLoad(Api* api, JNIEnv* env) override {
        api_ptr = api;
        env_ptr = env;
        LOGI("RustESP Modulu yuklendi (onLoad)");
    }

    void preAppSpecialize(AppSpecializeArgs* args) override {
        if (!api_ptr || !env_ptr || !args || !args->nice_name) {
            if (api_ptr) api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
            return;
        }

        const char* pkg = env_ptr->GetStringUTFChars(args->nice_name, nullptr);
        if (pkg && strcmp(pkg, "com.tencent.rmos") == 0) {
            is_target = true;
            LOGI("Hedef oyun tespit edildi: com.tencent.rmos");
        }
        if (pkg) {
            env_ptr->ReleaseStringUTFChars(args->nice_name, pkg);
        }

        if (!is_target) {
            api_ptr->setOption(Option::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const AppSpecializeArgs*) override {
        if (!is_target) return;

        LOGI("Oyun basariyla baslatildi, Zygisk enjeksiyonu aktif!");
    }
};

REGISTER_ZYGISK_MODULE(RustESPModule)