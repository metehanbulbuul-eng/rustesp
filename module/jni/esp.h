#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <android/log.h>

#define LOG_TAG "RustESP"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// === IL2CPP Tipleri (header'a gerek yok, manuel tanımlar) ===
typedef void* Il2CppDomain;
typedef void* Il2CppAssembly;
typedef void* Il2CppImage;
typedef void* Il2CppClass;
typedef void* Il2CppObject;
typedef void* Il2CppMethod;
typedef void* Il2CppField;
typedef void* Il2CppString;
typedef void* Il2CppArray;
typedef void* Il2CppType;

// === Vector yapıları ===
struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };

// === ESP çizim verisi ===
struct EspData {
    Vec2  screen_pos;      // Ekran koordinatları
    float distance;        // Oyuncuya mesafe
    std::string name;      // Oyuncu ismi
    float hp;              // Can
    float max_hp;          // Maksimum can
    int   team_id;         // Takım ID
    bool  is_dead;         // Ölü mü
    bool  is_local;        // Yerel oyuncu mu
    uint64_t role_id;      // Rol ID
};

// === Global değişkenler (main.cpp'de tanımlanacak) ===
extern uintptr_t g_il2cpp_base;
extern Il2CppClass* g_player_class;
extern Il2CppClass* g_camera_class;
extern Il2CppClass* g_entity_manager_class;

// === IL2CPP API fonksiyon işaretçileri ===
struct Il2CppApi {
    void* (*domain_get)();
    void* (*domain_assembly_open)(void* domain, const char* name);
    void* (*assembly_get_image)(void* assembly);
    void* (*class_from_name)(void* image, const char* namespaze, const char* name);
    void* (*class_get_method_from_name)(void* klass, const char* name, int args_count);
    void* (*class_get_field_from_name)(void* klass, const char* name);
    size_t (*field_get_offset)(void* field);
    void* (*object_new)(void* klass);
    void* (*runtime_invoke)(void* method, void* obj, void** params, void** exc);
    void* (*string_new)(const char* str);
    char* (*string_to_utf8)(void* str);
    void* (*array_new)(void* klass, size_t length);
    uint32_t (*array_length)(void* array);
    void* (*field_get_value_object)(void* obj, void* field);
    void  (*field_get_value)(void* obj, void* field, void* value);
    void* (*class_get_parent)(void* klass);
    const char* (*class_get_name)(void* klass);
    const char* (*class_get_namespace)(void* klass);
};

extern Il2CppApi api;

// === Yardımcı fonksiyonlar ===
static inline uintptr_t get_absolute(uintptr_t rva) {
    return g_il2cpp_base + rva;
}

// RVA'dan fonksiyon çağrısı için template
template<typename T>
static inline T call_rva(uintptr_t rva) {
    return reinterpret_cast<T>(g_il2cpp_base + rva);
}