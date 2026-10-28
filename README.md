# Multi-Channel UV Player

A Windows desktop player that maps content onto the UV channels of an FBX mesh and shows the result in a 3D view, in the spirit of a media server's stage view and timeline. The full concept lives in the project's concept doc.

Current state: **milestone 2, layers and LED mode**.

## Getting a build

Every push builds a Windows version automatically. On GitHub, open **Actions → Windows build**, pick the latest run, and download **MultiChannelUVPlayer-windows** at the bottom. Unzip it and run `MultiChannelUVPlayer.exe`. No installer is needed.

## What it does

- **Open an FBX** with any number of surfaces. Each object is a surface; an object with several materials is split into one surface per material. The mesh is converted to metres with Y up.
- **Mesh size and scale:** shows the file's units and the size in metres, with a scale factor for meshes that come in at the wrong size.
- **UV channels:** lists every channel with its name from the file and how much of the mesh sits inside 0-1. Channels are numbered from 1, so UV01 is the first channel in the file. Each mapping's size comes from the largest file on it.
- **Duplicate channels:** an exact copy of another channel is flagged, and you choose Keep or Ignore.
- **Clamp:** on by default, so content shows only inside the 0-1 UV square. Turn it off to repeat the image.
- **Layer stack:** one global stack, top to bottom. Each layer has an image, a mapping (UV channel), an optional surface, a blend mode (Normal, Add, Multiply, Screen, Mask) and an opacity. Drag rows to reorder, or use Up and Down. Up to 15 visible layers are drawn.
- **Warnings per layer:** a file smaller than its mapping, or with a different aspect ratio, is flagged.
- **Folder import:** each image becomes a layer. A file is placed when its name contains `UV` or `uv` followed by one or two digits, anywhere in the name (`UV03_logo.png`, `wall_uv3.png`). Files without a usable tag wait under *Unassigned files* for you to pick a mapping.
- **Projection and LED modes:** in Projection mode every channel is shared by the whole mesh. In LED mode UV01 is per surface, like feed mapping: a UV01 file must also name its surface (`Wall_1_UV1.png` goes on Wall 1; case, spaces and underscores are ignored). UV02 and up stay shared layouts such as the output map.
- **3D view:** shows the layers, the bare mesh, or a coloured test grid on any channel. Wireframe and floor grid toggles.
- **Drag and drop** FBX files, images or folders onto the window.

Supported images: PNG, JPG, TGA, BMP. Images larger than the GPU's texture limit (shown in the log) are refused with a message.

## Controls

| Action | Input |
| --- | --- |
| Orbit | Left drag |
| Pan | Right or middle drag |
| Zoom | Mouse wheel |
| Frame the mesh | F, or the Frame button |

## Command line

```
MultiChannelUVPlayer.exe [mesh.fbx] [images or folders...] [--led] [--grid N] [--wireframe]
```

`--led` starts in LED mode. `--grid N` shows the test grid on UV channel N. `--screenshot out.png` renders a few frames, saves the window and exits, which is used for automated checks.

## Building from source

Needs CMake 3.24+ and a recent Visual Studio (or any C++20 compiler). Dependencies (GLFW, Dear ImGui, ufbx, stb) are fetched by CMake; the OpenGL loader is in `third_party/glad`.

```
cmake -S . -B build
cmake --build build --config Release
```
