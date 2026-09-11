#include "esp.h"
#include "offsets.h"
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

uintptr_t g_il2cpp_base = 0;
Il2CppApi api = {};

// === libil2cpp.so'nun tam yolunu ve base adresini bul ===
static char g_il2cpp_path[512] = {0};

static int find_lib_callback(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size;
    (void)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        g_il2cpp_base = (uintptr_t)info->dlpi_addr;
        strncpy(g_il2cpp_path, info->dlpi_name, sizeof(g_il2cpp_path) - 1);
        g_il2cpp_path[sizeof(g_il2cpp_path) - 1] = '\0';
        return 1;
    }
    return 0;
}

// === IL2CPP API fonksiyonlarını dlopen + dlsym ile bul ===
bool init_il2cpp_api() {
    // 1. dl_iterate_phdr ile base + tam yol bul
    dl_iterate_phdr(find_lib_callback, nullptr);
    if (g_il2cpp_base == 0) {
        LOGE("libil2cpp.so base bulunamadi (dl_iterate_phdr)");
        return false;
    }
    LOGI("libil2cpp.so base: 0x%lx", (unsigned long)g_il2cpp_base);
    LOGI("libil2cpp.so path: %s", g_il2cpp_path);

    // 2. Tam yol ile dlopen et - doğru namespace'te handle alırız
    void* handle = dlopen(g_il2cpp_path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        LOGE("libil2cpp.so dlopen basarisiz: %s", dlerror());
        // Alternatif: RTLD_GLOBAL dene
        handle = dlopen(g_il2cpp_path, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            LOGE("libil2cpp.so dlopen (RTLD_GLOBAL) basarisiz: %s", dlerror());
            return false;
        }
    }
    LOGI("libil2cpp.so handle: %p", handle);

    // 3. dlsym ile fonksiyonları bul
    #define LOAD_API(name, type) \
        api.name = (type)dlsym(handle, "il2cpp_" #name); \
        if (!api.name) { LOGE("il2cpp_%s bulunamadi", #name); } \
        else { LOGI("il2cpp_%s OK (%p)", #name, api.name); }

    LOAD_API(domain_get, void*(*)())
    LOAD_API(domain_assembly_open, void*(*)(void*, const char*))
    LOAD_API(assembly_get_image, void*(*)(void*))
    LOAD_API(class_from_name, void*(*)(void*, const char*, const char*))
    LOAD_API(class_get_method_from_name, void*(*)(void*, const char*, int))
    LOAD_API(class_get_field_from_name, void*(*)(void*, const char*))
    LOAD_API(field_get_offset, size_t(*)(void*))
    LOAD_API(object_new, void*(*)(void*))
    LOAD_API(runtime_invoke, void*(*)(void*, void*, void**, void**))
    LOAD_API(string_new, void*(*)(const char*))
    LOAD_API(string_to_utf8, char*(*)(void*))
    LOAD_API(array_new, void*(*)(void*, size_t))
    LOAD_API(array_length, uint32_t(*)(void*))
    LOAD_API(field_get_value_object, void*(*)(void*, void*))
    LOAD_API(field_get_value, void(*)(void*, void*, void*))
    LOAD_API(class_get_parent, void*(*)(void*))
    LOAD_API(class_get_name, const char*(*)(void*))
    LOAD_API(class_get_namespace, const char*(*)(void*))

    #undef LOAD_API

    if (!api.domain_get || !api.class_from_name) {
        LOGE("Kritik API fonksiyonlari eksik!");
        return false;
    }
    LOGI("IL2CPP API hazir.");
    return true;
}

Il2CppClass* find_class(const char* namespaze, const char* name) {
    if (!api.domain_get || !api.domain_assembly_open) return nullptr;
    Il2CppDomain* domain = (Il2CppDomain*)api.domain_get();
    if (!domain) return nullptr;

    const char* assemblies[] = { "Assembly-CSharp.dll", "mscorlib.dll", nullptr };
    for (int i = 0; assemblies[i]; i++) {
        Il2CppAssembly* asm_ = (Il2CppAssembly*)api.domain_assembly_open(domain, assemblies[i]);
        if (!asm_) continue;
        Il2CppImage* image = (Il2CppImage*)api.assembly_get_image(asm_);
        if (!image) continue;
        Il2CppClass* klass = (Il2CppClass*)api.class_from_name(image, namespaze, name);
        if (klass) {
            LOGI("Sinif bulundu: %s.%s", namespaze, name);
            return klass;
        }
    }
    LOGE("Sinif bulunamadi: %s.%s", namespaze, name);
    return nullptr;
}

std::string read_string(void* il2cpp_str) {
    if (!il2cpp_str || !api.string_to_utf8) return "";
    char* cstr = api.string_to_utf8(il2cpp_str);
    if (!cstr) return "";
    std::string result(cstr);
    free(cstr);
    return result;
}