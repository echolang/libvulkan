/*
 * Runtime search for a Vulkan loader or MoltenVK. No vulkan.h.
 *
 * One ordered list: MoltenVK first (a LunarG loader without an ICD json
 * opens, then CreateInstance returns VK_ERROR_INCOMPATIBLE_DRIVER), then
 * VULKAN_SDK prefixes, then the remaining loader names.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>

static void *open_one(const char *name)
{
    return (void *)LoadLibraryA(name);
}

void *vk_sym(void *lib, const char *name)
{
    if (lib == NULL || name == NULL) {
        return NULL;
    }

    return (void *)GetProcAddress((HMODULE)lib, name);
}
#else
#include <dlfcn.h>

static void *open_one(const char *name)
{
    return dlopen(name, RTLD_NOW | RTLD_LOCAL);
}

void *vk_sym(void *lib, const char *name)
{
    if (lib == NULL || name == NULL) {
        return NULL;
    }

    return dlsym(lib, name);
}
#endif

static void *try_open(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    return open_one(name);
}

static void *try_sdk(const char *sdk, const char *rel)
{
    char buf[1024];
    int n;

    if (sdk == NULL || sdk[0] == '\0') {
        return NULL;
    }

    n = snprintf(buf, sizeof(buf), "%s/%s", sdk, rel);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return NULL;
    }

    return try_open(buf);
}

void *vk_open_loader(void)
{
    const char *sdk = getenv("VULKAN_SDK");
    void *h;
    int i;
    static const struct {
        int sdk;
        const char *path;
    } cands[] = {
        {0, "libMoltenVK.dylib"},
        {0, "/usr/local/lib/libMoltenVK.dylib"},
        {0, "/opt/homebrew/lib/libMoltenVK.dylib"},
        {1, "lib/libMoltenVK.dylib"},
        {1, "lib/libvulkan.1.dylib"},
        {1, "lib/libvulkan.so.1"},
        {1, "Lib/vulkan-1.dll"},
        {0, "libvulkan.1.dylib"},
        {0, "/opt/homebrew/lib/libvulkan.1.dylib"},
        {0, "/usr/local/lib/libvulkan.1.dylib"},
        {0, "libvulkan.so.1"},
        {0, "libvulkan.so"},
        {0, "vulkan-1.dll"},
    };

    for (i = 0; i < (int)(sizeof(cands) / sizeof(cands[0])); i++) {
        if (cands[i].sdk) {
            h = try_sdk(sdk, cands[i].path);
        } else {
            h = try_open(cands[i].path);
        }

        if (h != NULL) {
            return h;
        }
    }

    return NULL;
}
