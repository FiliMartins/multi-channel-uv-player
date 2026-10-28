# Multi-Channel UV Player

A Windows desktop player that maps content onto the UV channels of an FBX mesh and shows the result in a 3D view, in the spirit of a media server's stage view and timeline. The full concept lives in the project's concept doc.

This is **milestone 1: the viewer**.

## Getting a build

Every push builds a Windows version automatically. On GitHub, open **Actions → Windows build**, pick the latest run, and download **MultiChannelUVPlayer-windows** at the bottom. Unzip it and run `MultiChannelUVPlayer.exe`. No installer is needed.

## What milestone 1 does

- **Open an FBX** with any number of surfaces (objects). The mesh is converted to metres with Y up.
- **Mesh size and scale:** shows the file's units and the size in metres, with a scale factor for meshes that come in at the wrong size.
- **UV channels:** lists every channel with its name from the file and how much of the mesh sits inside 0-1. Channels are numbered from 1, so UV01 is the first channel in the file.
- **Duplicate channels:** an exact copy of another channel is flagged, and you choose Keep or Ignore.
- **Images on channels:** load one image per channel, or import a whole folder. A file is placed on a channel when its name contains `UV` or `uv` followed by one or two digits, anywhere in the name (`UV03_logo.png`, `wall_uv3.png`). Files without a usable tag wait under *Unassigned files* for you to pick a channel.
- **Clamp:** on by default, so content shows only inside the 0-1 UV square. Turn it off to repeat the image.
- **3D view:** pick the channel to show. A channel without an image shows a coloured test grid so its layout is readable. Wireframe and floor grid toggles.
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
MultiChannelUVPlayer.exe [mesh.fbx] [images or folders...] [--view N] [--wireframe]
```

`--view N` shows UV channel N on start. `--screenshot out.png` renders a few frames, saves the window and exits, which is used for automated checks.

## Building from source

Needs CMake 3.24+ and Visual Studio 2022 (or any C++20 compiler). Dependencies (GLFW, Dear ImGui, ufbx, stb) are fetched by CMake; the OpenGL loader is in `third_party/glad`.

```
cmake -S . -B build
cmake --build build --config Release
```
