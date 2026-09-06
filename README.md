# libvulkan

Vulkan is a C API with a Khronos XML registry. That is the OpenGL situation, not the Metal one. Metal needed an Objective-C shim because Echo cannot send `objc_msgSend`. Vulkan does not.

This is Echo's Vulkan binding: a generator, written in Echo, that reads the Khronos `vk.xml` checked in next to it and emits `src/vk.eco`. Every entry point is an address `vk::load` asks a getproc for. There is nothing to link. A forgotten `vk::load` is a `die`, not a jump to `0x0`.

This is also not a window. You still need GLFW (or whatever) to create a surface. This module talks to an ICD once one exists.

On Darwin the ICD is MoltenVK. `brew install molten-vk` (or a copy of `libMoltenVK.dylib` on the dlopen path) is enough. We do not vendor MoltenVK and we do not `#[link:]` it. The module already links `dl` on Linux. Windows is `LoadLibraryA("vulkan-1.dll")`. There is no `vulkan.h` anywhere in this tree.

## The simple case

```bash
epm add echolang/libvulkan --git https://github.com/echolang/libvulkan --range ^0.1
```

That is all for the table. For a window, add libglfw too.

```echo
if (!vk::available()) {
    die('no Vulkan ICD');
}

usize $n = vk::loadDefault();
vk::InstanceCreateInfo $info = vk::InstanceCreateInfo();
vk::Instance $instance = vk::Instance::none();
vk::Result $r = vk::CreateInstance(&$info, null, &$instance);
```

`available` is whether we could dlopen a loader or MoltenVK and resolve `vkGetInstanceProcAddr`. `loadDefault` does that and fills the global commands (`CreateInstance`, the enumerate-instance family). After `CreateInstance`, call `vk::loadInstance($instance)`. After `CreateDevice`, `vk::loadDevice($device)`. The later loads use `GetInstanceProcAddr` and `GetDeviceProcAddr` from the table the previous load filled, so the order is the API.

### Checking the result

Commands that return `VkResult` return `vk::Result`, an open integer enum. Named codes are `Result::success`, `Result::timeout`, `Result::errorDeviceLost`, … Unknown ICD codes become `other` and keep the raw value (`Result::from(-99)->value()` is still `-99`). The C function pointer is still `int32`; the `#[inline]` wrapper is the wrap.

`vk::check($r)` turns negatives into `Error::vulkan` and keeps the non-negative status codes (`timeout`, `notReady`, `incomplete`, `suboptimalKhr`).

```echo
vk::Result $r = vk::CreateInstance(&$info, null, &$instance);
vk::Result $ok = guard vk::check($r) else ($e) {
    die($e->message());
}
```

`SUCCESS` / `ERROR_*` constants remain: they name the `int32` that still lives in struct fields and `VkResult *` out-params. `Error::vulkan` carries that raw code. `Error::noIcd` is a missing loader, not a `VkResult`.

A missing getproc stays the die-stub the table was constructed with. Calling a function this ICD does not have is a programmer error. The die is the check you forgot to write.

```echo
vk::DestroyInstance($instance, null);
// die: vk: entry point called before vk::load, or unavailable in this ICD
```

That is a test, not a comment: `#[tests: expects death]`.

## Naming

Names are stripped the way ash / gl-rs do it. `vkCreateInstance` is `vk::CreateInstance`. `VK_SUCCESS` is `vk::SUCCESS`. `VkInstance` is `vk::Instance`. `VkResult` codes on the enum are lowerCamel (`Result::errorDeviceLost`). A constant that starts with a digit after the prefix gets an underscore (`VK_3D` is `_3D`), because Echo identifiers cannot start with a digit.

Parameter names that collide with Echo keywords become `pN` (`this` is `p3`). `pCreateInfo` is already fine and stays.

Handles are Echo structs wrapping a `uint64`, so a `Buffer` cannot sneak into a `Device` slot. The C function-pointer slot takes the word. The `#[inline]` wrapper peels `.handle`:

```echo
vk::DestroyInstance($instance, null);
// conceptually
// state::$commands->DestroyInstance($instance->handle, null);
```

`Instance::none()` is the null handle. `null` is an Echo keyword, so it is not a method name.

## Loading

Vulkan is three tables pretending to be one API.

1. Global, via `vkGetInstanceProcAddr(null, name)`: `CreateInstance`, `EnumerateInstance*`.
2. Instance, via `vkGetInstanceProcAddr(instance, name)`: physical devices, surfaces, `CreateDevice`, `GetDeviceProcAddr`.
3. Device, via `vkGetDeviceProcAddr(device, name)`: everything else.

`load` / `loadInstance` / `loadDevice` return how many slots resolved. A second load overwrites only what resolves this time. It cannot unbind. A null from getproc leaves the die-stub.

Here is the catch: load does not fail if half the table is missing. It counts. I understand some people want a result per name. I don't.

### loadDefault and the ICD

`loadDefault` dlopens a loader, `dlsym`s `vkGetInstanceProcAddr`, and fills the global table through a trampoline that calls that word with instance `0`.

The search is an ordered list, not a guess. MoltenVK first (`libMoltenVK.dylib`, Homebrew, `/usr/local`), then `$VULKAN_SDK` prefixes, then the remaining loader names (`libvulkan.1.dylib`, `libvulkan.so.1`, `vulkan-1.dll`). A LunarG `libvulkan.1.dylib` without an ICD json will open and then `CreateInstance` returns `ERROR_INCOMPATIBLE_DRIVER`. Direct MoltenVK is what `brew install molten-vk` actually installs, so we try that first.

`getInstanceProcAddrPtr` is the word we `dlsym`'d. Hand it to GLFW so `createWindowSurface` uses the same ICD:

```echo
glfw::initVulkanLoader(vk::getInstanceProcAddrPtr());
glfw::windowHint(glfw::CLIENT_API, glfw::NO_API);
```

Skip that on a brew-only Mac and GLFW opens `libvulkan.1.dylib` (or nothing), and you get two loaders or a surface that does not match the instance you created.

### Your own getproc

`load` takes a name-only getproc:

```echo
usize $n = vk::load(&myGetproc);
```

The type is `extern function<ptr<uint8>(ptr<const uint8>)>`. GLFW's function takes an instance plus a name. That will not compile, which is the point: `initVulkanLoader` is the GLFW hook, not a signature lie.

```echo
vk::load(&glfw::getInstanceProcAddress);
// will not compile: getInstanceProcAddress takes (uint64, ptr<const uint8>)
```

`loadDefault` uses the trampoline around the word it `dlsym`'d. You do not pass GLFW in.

### Portability on MoltenVK

Newer MoltenVK wants `VK_KHR_portability_enumeration` on the instance, flag `INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`, and `VK_KHR_portability_subset` on the device. Older copies (this machine's 2023 dylib) do not advertise that extension and will reject it. Enumerate instance extensions and only enable what is there. `examples/devices` does that.

Extension names are C strings. `ppEnabledExtensionNames` is `ptr<ptr<const uint8>>`. You take the address of a `cstr`. You do not pass the Echo string itself.

```echo
string $portability = 'VK_KHR_portability_enumeration';
ptr<const uint8> $ext0 = $portability->cstr();
$info->flags = vk::INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
$info->enabledExtensionCount = 1;
$info->ppEnabledExtensionNames:$ = &$ext0;
```

## Types

Structs are C layout. `sType` fields default to the matching `STRUCTURE_TYPE_*` constant. Pointer fields start as null. Integer fields start as 0.

```echo
vk::ApplicationInfo $app = vk::ApplicationInfo();
// $app->sType is STRUCTURE_TYPE_APPLICATION_INFO
// $app->pApplicationName is null
$app->apiVersion = vk::makeApiVersion(0, 1, 0, 0);
```

`makeApiVersion` packs the Vulkan version word. Variant `0` is the shipped API.

C arrays inside structs are `T[N]` (`uint8[256] $extensionName`, `uint8[16] $pipelineCacheUUID`). That is the C layout. Index them, or take `&$p->extensionName[0]` when you need a `char*`:

```echo
vk::PhysicalDeviceProperties $props = vk::PhysicalDeviceProperties();
vk::GetPhysicalDeviceProperties($physical, &$props);
ptr<uint8> $name = &$props->deviceName[0];
string $s = str::from(ptr<const uint8>($name:$));
```

Unions keep the largest member (first on a tie) and pad to the C union size. `ClearColorValue` is four floats. `ClearValue` is that color. The depth/stencil arm is gone; write a color clear, or put a `ClearDepthStencilValue` in a struct that actually has that field.

```echo
vk::ClearValue $clear = vk::ClearValue();
$clear->color->float32[0] = 0.25f;
$clear->color->float32[3] = 1.0f;
```

Wrappers are `#[inline]`. They do one thing: peel handles and call the function pointer in the table.

## Generating

The committed `src/vk.eco` is Vulkan **1.3** plus the WSI, portability, and debug extensions the generator defaults to:

`VK_KHR_surface`, `VK_KHR_swapchain`, `VK_KHR_portability_enumeration`, `VK_KHR_portability_subset`, `VK_EXT_debug_utils`, `VK_EXT_metal_surface`, `VK_MVK_macos_surface`, `VK_KHR_xcb_surface`, `VK_KHR_xlib_surface`, `VK_KHR_win32_surface`.

The generator is a target of this module, written in Echo:

```bash
echoc run --target generate
echoc run --target generate -- --check
```

The `--` is echoc's. Everything after it is argv for generate. If you drop it, `--check` is echoc's flag, not ours.

`--check` regenerates in memory, byte-compares against `src/vk.eco`, writes nothing, and exits 1 on drift. Generated source is a cache input, and Echo has no build-script step. I want the generated file committed and the check to fail the run, not a silent rewrite during compile.

`--ext` is repeatable and **adds** to that default set. You do not pass the WSI names again unless you are changing `defaultExts` in `gen/main.eco`. The banner at the top of `src/vk.eco` records the SHA and the invocation so you can see what you last asked for. It does not record a timestamp, so a byte compare stays stable.

If `src/vk.eco` is stale enough to refuse to compile, delete it and rerun. `src/loader.eco` is hand-written, names no generated symbol, and keeps the `src/*.eco` glob non-empty.

```bash
rm src/vk.eco
echoc run --target generate
```

Flags:

```
--xml PATH        Khronos registry (default vk.xml)
--out PATH        generated module (default src/vk.eco)
--api NAME        api to fold (default vulkan)
--version X.Y     maximum version (default 1.3)
--ext NAME        extra extension, repeatable (defaults already include WSI)
--check           regenerate in memory, compare, write nothing
```

`--api` is `vulkan`. The fold walks features whose version is at most the one you asked for, then each extension. Sorted name order is the one canonical order for everything emit writes.

## The registry

`vk.xml` is pinned to Vulkan-Headers `v1.4.361` (`31386378257ac8653ce5b32c93baec385259ebbe`). Regeneration is deliberate. To bump it:

```bash
curl -L -o vk.xml https://raw.githubusercontent.com/KhronosGroup/Vulkan-Headers/<SHA>/registry/vk.xml
```

The SHA lives in two places: this README and `REGISTRY_SHA` in `gen/main.eco`. Put the new one in both, then regenerate. The banner of `src/vk.eco` has to match.

## Tests

```bash
cd libvulkan
echoc test
```

GPU-free tests always. `#[group: "vulkan"]` talks to an ICD. They return if `available()` is false, so a VM without MoltenVK does not look like an API bug.

Each test is a fresh process. `load` in one test cannot leak into the next.

## Examples

```bash
cd examples
echoc run --target devices
echoc run --target clear -- --frames 120
echoc run --target triangle -- --frames 120
```

`devices` opens no window. It creates an instance, lists physical devices, and prints the names. On this Mac that is `Apple M2 Max`.

`clear` and `triangle` open a GLFW window with `NO_API`, create a swapchain, and close themselves after N frames (`--frames`, default 300). `triangle` loads committed SPIR-V (`triangle.vert.spv` / `triangle.frag.spv`). The GLSL next to them is the source; `glslangValidator -V` regenerates the `.spv` files.

Both call `glfw::initVulkanLoader(vk::getInstanceProcAddrPtr())` before `glfw::init`, so the surface comes from the same ICD `loadDefault` opened.
