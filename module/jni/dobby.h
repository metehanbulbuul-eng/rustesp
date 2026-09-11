#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Dobby hook API
int DobbyHook(void *address, void *fake_func, void **out_original_func);
int DobbyDestroy(void *address);

// Alternatif isimler
int dobby_hook(void *address, void *fake_func, void **out_original_func);

#ifdef __cplusplus
}
#endif