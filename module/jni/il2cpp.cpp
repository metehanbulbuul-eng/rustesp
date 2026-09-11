#include "esp.h"
#include "offsets.h"
#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>

uintptr_t g_il2cpp_base = 0;
Il2CppApi api = {};

static char g_il2cpp_path[512] = {0};

static int find_lib_callback(struct dl_phdr_info* info, size_t size, void* data) {
    (void)size; (void)data;
    if (info->dlpi_name && strstr(info->dlpi_name, "libil2cpp.so")) {
        g_il2cpp_base = (uintptr_t)info->dlpi_addr;
        strncpy(g_il2cpp_path, info->dlpi_name, sizeof(g_il2cpp_path) - 1);
        g_il2cpp_path[sizeof(g_il2cpp_path) - 1] = '\0';
        return 1;
    }
    return 0;
}

// === ELF'ten sembol ara (GNU hash tabanlı, güvenli) ===
static void* find_symbol_in_elf(uintptr_t base, const char* symbol_name) {
    Elf64_Ehdr* ehdr = (Elf64_Ehdr*)base;
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return nullptr;

    Elf64_Phdr* phdr = (Elf64_Phdr*)(base + ehdr->e_phoff);
    uintptr_t dyn_addr = 0;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_DYNAMIC) {
            dyn_addr = base + phdr[i].p_vaddr;
            break;
        }
    }
    if (!dyn_addr) return nullptr;

    Elf64_Dyn* dyn = (Elf64_Dyn*)dyn_addr;
    uintptr_t symtab = 0, strtab = 0, gnu_hash = 0;
    size_t strsz = 0;

    for (Elf64_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB:   symtab   = d->d_un.d_ptr; break;
            case DT_STRTAB:   strtab   = d->d_un.d_ptr; break;
            case DT_STRSZ:    strsz    = d->d_un.d_val; break;
            case DT_GNU_HASH: gnu_hash = d->d_un.d_ptr; break;
        }
    }

    if (strtab < base) strtab += base;
    if (symtab < base) symtab += base;
    if (gnu_hash < base && gnu_hash != 0) gnu_hash += base;

    if (!strtab || !symtab || !gnu_hash) return nullptr;

    // GNU hash'ten gerçek sembol sayısını bul
    uint32_t* gh = (uint32_t*)gnu_hash;
    uint32_t nbuckets    = gh[0];
    uint32_t symoffset   = gh[1];
    uint32_t bloom_size  = gh[2];
    uint64_t* bloom      = (uint64_t*)&gh[4];
    uint32_t* buckets    = (uint32_t*)&bloom[bloom_size];
    uint32_t* chain      = &buckets[nbuckets];

    uint32_t max_sym = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (buckets[i] > max_sym) max_sym = buckets[i];
    }

    if (max_sym >= symoffset) {
        uint32_t* c = &chain[max_sym - symoffset];
        while (!(*c & 1)) {
            max_sym++;
            c++;
        }
    }
    size_t num_syms = max_sym + 1;

    const char* str = (const char*)strtab;
    Elf64_Sym* sym  = (Elf64_Sym*)symtab;

    for (size_t i = 0; i < num_syms; i++) {
        if (sym[i].st_name == 0 || sym[i].st_name >= strsz) continue;
        const char* name = str + sym[i].st_name;
        if (strcmp(name, symbol_name) == 0) {
            return (void*)(base + sym[i].st_value);
        }
    }
    return nullptr;
}

// === Tüm il2cpp API'lerini ELF'ten bul ===
bool init_il2cpp_api() {
    dl_iterate_phdr(find_lib_callback, nullptr);
    if (g_il2cpp_base == 0) {
        LOGE("libil2cpp.so base bulunamadi");
        return false;
    }
    LOGI("libil2cpp.so base: 0x%lx", (unsigned long)g_il2cpp_base);

    #define LOAD_API(name, type) \
        LOGI("Araniyor: il2cpp_%s", #name); \
        api.name = (type)find_symbol_in_elf(g_il2cpp_base, "il2cpp_" #name); \
        if (!api.name) { LOGE("  -> BULUNAMADI"); } \
        else { LOGI("  -> OK (%p)", api.name); }

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