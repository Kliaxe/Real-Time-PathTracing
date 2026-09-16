# Real-Time Path Tracing

![Renderer screenshot](Screenshots/Screenshot%201.png)

Real-Time PathTracing is a personal Vulkan renderer for real-time path tracing, ReSTIR PT Enhanced path resampling, physically based shading, and NVIDIA Real-Time Denoisers.

It started as a master's project focused on real-time path tracing with ReSTIR direct illumination, and continues as an evolving personal rendering project now built around ReSTIR PT Enhanced (Lin, Kettunen & Wyman, I3D 2026).

## Features

- ReSTIR PT Enhanced path resampling, alongside a reference path tracer
- DLSS Ray Reconstruction and NVIDIA NRD denoising
- Disney-style BSDF with diffuse, metallic, glass, and clearcoat lobes
- HDRI environment and emissive light importance sampling
- Per-pass GPU profiler and headless capture comparisons

## Tested Setup

The owned Vulkan runtime has been tested on Windows with:

- CPU: Intel Core Ultra 9 275HX
- GPU: NVIDIA GeForce RTX 5080 Laptop GPU
- VRAM: 16,303 MiB
- NVIDIA driver: 592.01

Linux configure/build presets are included; Linux GPU runtime validation remains pending on a compatible machine. The captures and timings currently in this repository were produced on the setup above.

## Requirements

The expected development setup is:

- Windows 11 or Linux
- Visual Studio Code with the CMake Tools and C/C++ extensions
- CMake 3.23 or newer
- LLVM MinGW (UCRT runtime), which provides Clang, LLDB, and the C++ runtime
- Ninja
- A current Vulkan driver with ray tracing, compute shader derivatives, and indirect ray tracing support
- Vulkan SDK 1.4.341 or newer when running the validation-layer test matrix
- Git LFS
- Internet access during first CMake configure

Install the standalone compiler and build tool without installing Visual Studio:

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --exact --scope user
winget install --id Ninja-build.Ninja --exact --scope user
.\Scripts\Bootstrap-LLVM-MinGW.ps1
```

The bootstrap script creates a stable LLVM MinGW path used by the CMake presets, so CMake Tools does not depend on the VS Code process inheriting WinGet's versioned `PATH`. Rerun it after updating LLVM MinGW.

The first configure downloads pinned Vulkan-Headers, Volk, VMA, GLFW, GLM, Dear ImGui, fmt, tinygltf, stb, DXC, and NVIDIA NRD revisions, and on Windows the NVIDIA Streamline SDK (see [DLSS Ray Reconstruction](#dlss-ray-reconstruction)). Compilation does not use headers from the machine Vulkan SDK. The content assets are managed through Git LFS, so run this after cloning if the assets are missing:

```powershell
git lfs pull
```

## Build

Use the CMake presets directly. VS Code CMake Tools invokes these same commands.

```powershell
cmake --preset x64-Release
cmake --build --preset x64-Release --target RealTimePathTracing
```

For a normal debug build:

```powershell
cmake --preset x64-Debug
cmake --build --preset x64-Debug --target RealTimePathTracing
```

Linux uses the equivalent `linux-Debug` and `linux-Release` presets. Install the platform development packages required by GLFW for the selected Wayland/X11 backend before configuring.

All project shaders are HLSL compiled by the pinned DXC release to embedded SPIR-V during the build. Shader changes require a rebuild and application restart.

The executable is written to:

```text
Binaries\Release\RealTimePathTracing.exe
```

Use `Binaries\Debug\RealTimePathTracing.exe` for a debug build.

Build and run the focused CPU, Vulkan, ray-tracing, and presentation checks with:

```powershell
cmake --build --preset x64-Debug --target RtptImageDataTests RtptCameraTests RtptVulkanBootstrapTests RtptPresentationTests
ctest --preset x64-Debug
```

On Windows, set the `RTPT_VULKAN_VALIDATION_BIN` CMake cache variable to the Vulkan SDK `Bin` directory when the validation layer is not registered globally or the registered layer is older than 1.4.341. Windowed validation fails fast on an older layer because it cannot validate the required KHR surface/swapchain-maintenance path.

## Visual Studio Code

After reopening VS Code, use the CMake Tools status bar to select:

1. Configure preset: `x64-Debug`
2. Launch target: `RealTimePathTracing`

Then select **Run and Debug** and launch `RealTimePathTracing (CMake Tools)`. The launch configuration builds through CMake Tools and debugs with the LLVM MinGW LLDB adapter.

## Run

To open the renderer:

```powershell
.\Binaries\Release\RealTimePathTracing.exe
```

The application UI exposes the scene selection, render mode, ReSTIR PT settings, path tracing settings, and denoising options used during the project.

## DLSS Ray Reconstruction

Both path tracers can denoise with either NVIDIA NRD or DLSS Ray Reconstruction, chosen in the **Resolve** dropdown (`--resolve-mode denoise` or `--resolve-mode denoise-rr` on the command line). Ray Reconstruction runs at native resolution (DLAA) and needs Windows and an NVIDIA RTX GPU; elsewhere the option stays disabled with the reason on hover, and `denoise-rr` falls back to NRD.

It is reached through [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) v2.14.1. The NGX SDK underneath only ships MSVC static libraries, which an LLVM MinGW build cannot link, while Streamline is a set of DLLs with a C API that it can load at runtime. Configure downloads the pinned Streamline release (about 276 MB, once per build directory) and the build copies only the four runtime DLLs next to the executable: the development DLLs for Debug, the NVIDIA-signed production DLLs otherwise, whose signature is checked before loading.

Streamline's source is MIT licensed, but `nvngx_dlssd.dll`, the DLSS model itself, is covered by the NVIDIA RTX SDKs license, which is copied next to the DLLs as `nvngx_dlss.license.txt`. None of these binaries are committed to this repository. To build without Streamline:

```powershell
cmake --preset x64-Release -D REAL_TIME_PATH_TRACING_WITH_STREAMLINE=OFF
```

## Repository Layout

- `Source/Framework/`: project-owned platform, Vulkan execution/resource, swapchain, and ImGui integration.
- `Source/`: application orchestration, scene code, renderers, denoising integration, and HLSL shaders.
- `Content/`: models, textures, and HDRIs used by the renderer.
- `docs/`: project and agent documentation.
- `Scripts/`: toolchain bootstrap and capture/comparison helpers.

## Scope Notes

ReSTIR is applied to whole paths rather than to direct lighting alone: direct and global illumination share one reservoir, and reuse happens through shift mappings between pixels. The code also includes denoising because real-time ray traced output is normally judged together with a reconstruction step.

The reference path tracer is kept alongside it, because the correctness gate for the resampler is that it converges to the same image.

The timings are measurements from this implementation on the tested machine. They are mainly useful for comparing the modes in this project.

## Troubleshooting

If the program opens with missing scenes or textures, check that Git LFS downloaded the content assets:

```powershell
git lfs pull
```

If CMake configure fails, check that Clang, Ninja, Git, and the platform packages required by GLFW are visible from a newly opened terminal. The first configure can take longer because pinned dependencies and DXC are downloaded and built.

For performance captures, prefer a Release build. Debug builds are useful for development and can produce misleading timings.

## Screenshots

![Renderer screenshot 2](Screenshots/Screenshot%202.png)

![Renderer screenshot 3](Screenshots/Screenshot%203.png)

![Renderer screenshot 4](Screenshots/Screenshot%204.png)
