#define _GNU_SOURCE
#include "esp.h"
#include "offsets.h"
#include <dlfcn.h>
#include <link.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

// Global değişkenler
uintptr_t g_il2cpp_base = 0;
Il2CppApi api = {};

// === IL2CPP API fonksiyonlarını dlsym ile bağla ===
bool init_il2cpp_api() {
    void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) {
        LOGE("libil2cpp.so yuklu degil!");
        return false;
    }

    struct link_map* map = nullptr;
    if (dlinfo(handle, RTLD_DI_LINKMAP, &map) != 0 || !map) {
        LOGE("link_map alinamadi!");
        return false;
    }
    g_il2cpp_base = (uintptr_t)map->l_addr;
    LOGI("libil2cpp.so base: 0x%lx", (unsigned long)g_il2cpp_base);

    #define LOAD_API(name, type) \
        api.name = (type)dlsym(handle, "il2cpp_" #name); \
        if (!api.name) { LOGE("il2cpp_%s bulunamadi", #name); }

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

// === Belirli bir sınıfı bul ===
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

// === String okuma ===
std::string read_string(void* il2cpp_str) {
    if (!il2cpp_str || !api.string_to_utf8) return "";
    char* cstr = api.string_to_utf8(il2cpp_str);
    if (!cstr) return "";
    std::string result(cstr);
    free(cstr);
    return result;
}