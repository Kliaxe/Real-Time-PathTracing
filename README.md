# Real-Time Path Tracing

![Renderer screenshot](Screenshots/Screenshot%201.png)

This repository contains the renderer and report material for a master's thesis about real-time path tracing with ReSTIR direct illumination. The project uses Vulkan ray tracing, physically based shading, NVIDIA Real-Time Denoisers, and a small experiment runner for capturing the images and timings used in the thesis.

The code is built around the thesis work. It should make the tested ideas possible to inspect, run, and discuss.

## Tested Setup

The project has been tested on Windows with:

- CPU: Intel Core Ultra 9 275HX
- GPU: NVIDIA GeForce RTX 5080 Laptop GPU
- VRAM: 16,303 MiB
- NVIDIA driver: 592.01

Other Vulkan ray tracing capable NVIDIA GPUs may work. The thesis results and timings were produced on the setup above.

## Requirements

The expected development setup is:

- Windows
- Visual Studio 2022 with C++ tools
- CMake
- Ninja
- Vulkan SDK
- Git LFS
- Internet access during first CMake configure

The first configure downloads external dependencies such as `nvpro_core2` and NVIDIA NRD. The content assets are managed through Git LFS, so run this after cloning if the assets are missing:

```powershell
git lfs pull
```

## Build

The PowerShell scripts are the easiest path on Windows. They enter the Visual Studio developer environment and call the same CMake presets used by the project.

```powershell
.\Scripts\Setup-CMake.ps1 -Config Release -Build -FirstFailureOnly
```

For a normal debug build:

```powershell
.\Scripts\Setup-CMake.ps1 -Config Debug -Build -FirstFailureOnly
```

The same build can also be done manually with CMake:

```powershell
cmake --preset x64-Release
cmake --build --preset x64-Release --target RealTimePathTracing
```

The executable is written to:

```text
Binaries\Release\RealTimePathTracing.exe
```

Use `Binaries\Debug\RealTimePathTracing.exe` for a debug build.

## Run

To open the renderer:

```powershell
.\Binaries\Release\RealTimePathTracing.exe
```

The application UI exposes the scene selection, render mode, ReSTIR settings, path tracing settings, and denoising options used during the project.

## Run Thesis Experiments

The renderer also has a small headless experiment runner for capturing repeatable images, metadata, and GPU timings.

List available experiment groups:

```powershell
.\Binaries\Release\RealTimePathTracing.exe --list-experiments
```

Run a quick smoke experiment:

```powershell
.\Binaries\Release\RealTimePathTracing.exe --experiment restir-smoke
```

Run a specific experiment and choose the output folder:

```powershell
.\Binaries\Release\RealTimePathTracing.exe --experiment restir-di-minimal --experiment-output Results\restir-di-minimal
```

Experiment output is written under `Results/` by default. Each run writes captures and small metadata files, and the summary files contain the GPU timings used by the report.

## Repository Layout

- `Source/`: application code, Vulkan renderer, path tracer, ReSTIR DI implementation, denoising integration, shaders, and experiment runner.
- `Content/`: models, textures, and HDRIs used by the renderer.
- `Report/`: LaTeX thesis source, report images, and report helper scripts.
- `Results/`: generated experiment captures and timing data.
- `Docs/`: planning notes, references, and thesis support material.
- `Scripts/`: CMake setup, build, and cleanup helpers.

## Scope Notes

The implementation is intentionally thesis-focused. ReSTIR is treated mainly as a direct illumination technique, while secondary path bounces are included to show how it can sit inside a path-tracing-style renderer. The code also includes denoising because real-time ray traced output is normally judged together with a reconstruction step.

The timings in the report are measurements from this implementation on the tested machine. They are mainly useful for comparing the modes in this project.

## Troubleshooting

If the program opens with missing scenes or textures, check that Git LFS downloaded the content assets:

```powershell
git lfs pull
```

If CMake configure fails, check that Visual Studio C++ tools, Ninja, and the Vulkan SDK are installed and visible from the terminal. The first configure can take longer because dependencies are downloaded and built.

For performance captures, prefer a Release build. Debug builds are useful for development and can produce misleading timings.

## Screenshots

![Renderer screenshot 2](Screenshots/Screenshot%202.png)

![Renderer screenshot 3](Screenshots/Screenshot%203.png)

![Renderer screenshot 4](Screenshots/Screenshot%204.png)
