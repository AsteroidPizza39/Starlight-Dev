# Starlight Familiarization Notes

This document captures the practical architecture and feature touchpoints for Starlight so future feature work can start with minimal rediscovery.

## Core startup and lifecycle

- `src/Main.cpp` is the process entrypoint.
  - Initializes crash/signal handling.
  - Calls `Editor::Initialize()` (stage 1, no RomFS dependency).
  - Calls `Editor::InitializeRomFSPathDependant()` (stage 2, RomFS-dependent systems).
  - Runs the render loop via `UIMgr::Render()` until `UIMgr::ShouldWindowClose()`.
  - Shuts everything down through `Editor::Shutdown()`.
- `src/Editor.cpp` is the bootstrap coordinator.
  - Creates `WorkingDir`, `WorkingDir/Cache`, `WorkingDir/Projects`, and `WorkingDir/Templates`.
  - Initializes core managers and UI systems.
  - Loads persisted tool config (`Config.epathcfg`, `Project.eprojcfg`, `Identity.egit`, additional AINB node definitions).
  - During stage 2, initializes Zstd dictionaries from RomFS, detects game/data versions, loads actor/plugin/data systems, and prepares BFRES defaults.

## UI orchestration and tool entrypoints

- `src/manager/UIMgr.cpp` owns window creation (GLFW/OpenGL/ImGui), frame rendering, and main menu routing.
- Main menu flow:
  - `Settings` updates path config and may trigger stage 2 initialization.
  - `Projects` handles add/select/export flows.
  - `Tools` opens feature windows: Map Editor, AINB Editor, Actor Editor, Collision Generator, Plugins.
- Window lifetime and project-switch safety are managed centrally in `UIMgr` (`SupportsProjectChange()` gates switching).

## Project/workspace model

- Working root is relative to process current directory (`FileUtil::gCurrentDirectory`), expected to be `rootDir` at runtime.
- Project data lives in:
  - `WorkingDir/Projects/<project>/romfs/...`
- Project operations are in `src/manager/ProjectMgr.cpp`:
  - `AddProject` creates project `romfs`.
  - `SelectProject` clears in-memory caches and reloads project-sensitive data.
  - `ExportProject` optionally regenerates RSTB, then copies project `romfs` subtree into the user-selected export path.

## Overlay path resolution (critical behavior)

- `src/util/FileUtil.cpp` defines the read/write policy:
  - `GetRomFSFilePath(local, replaceable=true)` prefers project overlay path if the file exists there, otherwise falls back to base RomFS.
  - `GetSaveFilePath(local)` always points into selected project `romfs`.
- This overlay model is fundamental: feature changes must preserve it to avoid accidental writes to base RomFS.

## Game file I/O surfaces

- Binary format handlers are grouped under `src/file/game/`:
  - `byml/` (BYML/BGYML)
  - `sarc/` (SARC)
  - `bfres/` and `texture/` (model/texture formats)
  - `terrain/` (terrain archives)
  - `phive/` (physics/navmesh/static-compound related formats)
  - `restbl/` + `zstd/` (resource table and compression)
- Tool-specific config/state files are under `src/file/tool/`.

## Export and RSTB generation path

- Export entrypoint: `ProjectMgr::ExportProject(bool generateRSTB)`.
  - When enabled, it calls `tool/ResourceSizeTableGenerator::Generate()`.
  - Then copies `GetSaveFilePath()` recursively to export destination.
- `src/tool/ResourceSizeTableGenerator.cpp` behavior:
  - Loads base game RSTB from RomFS for detected internal version.
  - Walks all files in selected project `romfs`.
  - Computes per-file sizes (format-specific logic and alignment rules).
  - Writes updated compressed RSTB under project `System/Resource/...`.

## Build and run baseline

- Build system:
  - Top-level `CMakeLists.txt` + `src/CMakeLists.txt`.
  - Presets in `CMakePresets.json` use `Ninja` + MSVC `cl.exe` on Windows.
- Preset examples:
  - `cmake --preset x64-debug`
  - `cmake --preset x64-release`
  - `cmake --build out/build/x64-release`
- Run expectation:
  - Working directory should be `rootDir` so `Assets/` and `WorkingDir/` resolve correctly.

## Practical change-routing guide

- If a feature starts in UI: begin in `src/rendering/...`, then trace into `src/manager/...`.
- If a feature reads/writes game data: trace from manager into `src/file/game/...`, and verify save destination with `FileUtil`.
- If feature affects export/package output: inspect `ProjectMgr::ExportProject` and `ResourceSizeTableGenerator`.
- Keep layering clean:
  - UI behavior: `rendering/`
  - Orchestration/state: `manager/` and `tool/`
  - Binary format details: `file/game/`
