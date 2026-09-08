# Real-Time Path Tracing

![Renderer screenshot](Screenshots/Screenshot%201.png)

Real-Time PathTracing is a personal Vulkan renderer for real-time path tracing, ReSTIR PT Enhanced path resampling, physically based shading, and NVIDIA Real-Time Denoisers.

It started as a master's project focused on real-time path tracing with ReSTIR direct illumination, and continues as an evolving personal rendering project now built around ReSTIR PT Enhanced (Lin, Kettunen & Wyman, I3D 2026).

## Tested Setup

The project has been tested on Windows with:

- CPU: Intel Core Ultra 9 275HX
- GPU: NVIDIA GeForce RTX 5080 Laptop GPU
- VRAM: 16,303 MiB
- NVIDIA driver: 592.01

Other Vulkan ray tracing capable NVIDIA GPUs may work. The captures and timings currently in this repository were produced on the setup above.

## Requirements

The expected development setup is:

- Windows
- Visual Studio Code with the CMake Tools and C/C++ extensions
- CMake 3.23 or newer
- LLVM MinGW (UCRT runtime), which provides Clang, LLDB, and the C++ runtime
- Ninja
- Vulkan SDK
- Git LFS
- Internet access during first CMake configure

Install the standalone compiler and build tool without installing Visual Studio:

```powershell
winget install --id MartinStorsjo.LLVM-MinGW.UCRT --exact --scope user
winget install --id Ninja-build.Ninja --exact --scope user
.\Scripts\Bootstrap-LLVM-MinGW.ps1
```

The bootstrap script creates a stable LLVM MinGW path used by the CMake presets, so CMake Tools does not depend on the VS Code process inheriting WinGet's versioned `PATH`. Rerun it after updating LLVM MinGW.

The first configure downloads external dependencies such as `nvpro_core2` and NVIDIA NRD. The content assets are managed through Git LFS, so run this after cloning if the assets are missing:

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

The executable is written to:

```text
Binaries\Release\RealTimePathTracing.exe
```

Use `Binaries\Debug\RealTimePathTracing.exe` for a debug build.

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

## Repository Layout

- `Source/`: application code, Vulkan renderer, path tracer, ReSTIR PT Enhanced implementation, denoising integration, and shaders.
- `Content/`: models, textures, and HDRIs used by the renderer.
- `docs/`: project and agent documentation.
- `Scripts/`: standalone LLVM MinGW bootstrap helper.

## Scope Notes

ReSTIR is applied to whole paths rather than to direct lighting alone: direct and global illumination share one reservoir, and reuse happens through shift mappings between pixels. The code also includes denoising because real-time ray traced output is normally judged together with a reconstruction step.

The reference path tracer is kept alongside it, because the correctness gate for the resampler is that it converges to the same image.

The timings are measurements from this implementation on the tested machine. They are mainly useful for comparing the modes in this project.

## Troubleshooting

If the program opens with missing scenes or textures, check that Git LFS downloaded the content assets:

```powershell
git lfs pull
```

If CMake configure fails, check that LLVM MinGW, Ninja, and the Vulkan SDK are installed and visible from a newly opened terminal. The first configure can take longer because dependencies are downloaded and built.

For performance captures, prefer a Release build. Debug builds are useful for development and can produce misleading timings.

## Screenshots

![Renderer screenshot 2](Screenshots/Screenshot%202.png)

![Renderer screenshot 3](Screenshots/Screenshot%203.png)

![Renderer screenshot 4](Screenshots/Screenshot%204.png)
