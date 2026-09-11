#include "esp.h"
#include "offsets.h"
#include <dlfcn.h>
#include <cstring>
#include <cstdio>

// Global değişkenler
uintptr_t g_il2cpp_base = 0;
Il2CppApi api = {};

// === IL2CPP API fonksiyonlarını dlsym ile bağla ===
bool init_il2cpp_api() {
    // libil2cpp.so'nun yüklü olduğundan emin ol
    void* handle = dlopen("libil2cpp.so", RTLD_NOLOAD | RTLD_NOW);
    if (!handle) {
        LOGE("libil2cpp.so yüklü değil!");
        return false;
    }

    // Base adresi al (link_map üzerinden)
    struct link_map* map = nullptr;
    dlinfo(handle, RTLD_DI_LINKMAP, &map);
    if (map) {
        g_il2cpp_base = (uintptr_t)map->l_addr;
        LOGI("libil2cpp.so base: 0x%lx", (unsigned long)g_il2cpp_base);
    } else {
        LOGE("link_map alınamadı!");
        return false;
    }

    // API fonksiyonlarını dlsym ile al
    #define LOAD_API(name) \
        api.name = (decltype(api.name))dlsym(handle, "il2cpp_" #name); \
        if (!api.name) { LOGE("il2cpp_%s bulunamadı", #name); }

    LOAD_API(domain_get);
    LOAD_API(domain_assembly_open);
    LOAD_API(assembly_get_image);
    LOAD_API(class_from_name);
    LOAD_API(class_get_method_from_name);
    LOAD_API(class_get_field_from_name);
    LOAD_API(field_get_offset);
    LOAD_API(object_new);
    LOAD_API(runtime_invoke);
    LOAD_API(string_new);
    LOAD_API(string_to_utf8);
    LOAD_API(array_new);
    LOAD_API(array_length);
    LOAD_API(field_get_value_object);
    LOAD_API(field_get_value);
    LOAD_API(class_get_parent);
    LOAD_API(class_get_name);
    LOAD_API(class_get_namespace);

    #undef LOAD_API

    if (!api.domain_get || !api.class_from_name) {
        LOGE("Kritik API fonksiyonları eksik!");
        return false;
    }

    LOGI("IL2CPP API hazır.");
    return true;
}

// === Belirli bir sınıfı bul ===
Il2CppClass* find_class(const char* namespaze, const char* name) {
    if (!api.domain_get || !api.domain_assembly_open) return nullptr;
    
    Il2CppDomain* domain = api.domain_get();
    if (!domain) return nullptr;

    // mscorlib.dll ve Assembly-CSharp.dll deneyelim
    const char* assemblies[] = { "Assembly-CSharp.dll", "mscorlib.dll", nullptr };
    
    for (int i = 0; assemblies[i]; i++) {
        Il2CppAssembly* asm_ = api.domain_assembly_open(domain, assemblies[i]);
        if (!asm_) continue;
        
        Il2CppImage* image = api.assembly_get_image(asm_);
        if (!image) continue;
        
        Il2CppClass* klass = api.class_from_name(image, namespaze, name);
        if (klass) {
            LOGI("Sınıf bulundu: %s.%s", namespaze, name);
            return klass;
        }
    }
    
    LOGE("Sınıf bulunamadı: %s.%s", namespaze, name);
    return nullptr;
}

// === Oyuncu pozisyonunu oku ===
bool read_player_position(void* player_obj, Vec3* out) {
    if (!player_obj || !out) return false;
    
    // get_PosX_Smooth, get_PosY_Smooth, get_PosZ_Smooth metodlarını kullan
    // Aslında doğrudan field okumak daha hızlı:
    // _PosX_Smooth = 0x5c, _PosY_Smooth = 0x60, _PosZ_Smooth = 0x64
    // Ama dumper offset'leri field offsetleri değil, metod RVA'ları.
    // Şimdilik metodları çağıralım.
    
    // get_PosX_Smooth
    void* method_x = call_rva<void*(*)(void*, const char*, int)>(
        RVA_Player_get_PosX_Smooth)(nullptr);  // Bu satır yanlış, düzelt
    return false;
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

// === Dictionary entry'yi oku ===
// Entry struct: { int hashCode; int next; long key; void* value; }
// Toplam 24 byte (0x18)
struct DictEntry {
    int32_t hashCode;  // 0x00
    int32_t next;      // 0x04
    int64_t key;       // 0x08
    void*   value;     // 0x10
};

// === ArrayBase element okuma ===
// C# array: [klass(8)][monitor(8)][bounds(8)][max_length(8)][data...]
// data başlangıcı: 0x20
void* array_get_element(void* array, size_t index, size_t elem_size) {
    if (!array) return nullptr;
    uint8_t* base = (uint8_t*)array + ARRAY_FIRST_ELEMENT_OFFSET;
    return base + (index * elem_size);
}