# Starlight

Starlight is a comprehensive, standalone toolkit designed for **The Legend of Zelda: Tears of the Kingdom** modding. It streamlines complex technical workflows, providing creators with the tools necessary to modify world data, AI logic, and physics structures without relying on heavy external dependencies.


## Key Features

* **Standalone Collision Generation:** Generate physics collision data directly within Starlight, eliminating the need for convoluted third-party pipelines or an external emulator.
* **Navmesh Generation:** Create and bake navigation meshes for NPCs and entities to ensure seamless movement within modified environments (single scene only).
* **Integrated Map Editor:**
    * **Terrain Viewing:** Real-time visualization of world terrain.
    * **Terrain Editing:** Modify heightmaps and terrain structures directly within the map environment.
* **AINB Editor:** A dedicated interface for editing the game’s AI logic files.
    * *Note:* The **Auto Layout** feature for node organization is currently in active development. While functional, manual adjustment may still be required for complex graphs.


## Getting Started

Starlight can be obtained either by building the project from the source code or by downloading pre-compiled binaries.
Please make sure to restart Starlight *after* you have specified the paths for the first time.

### Building from Source

**Windows (recommended path)**  
1. Install [Visual Studio 2022](https://visualstudio.microsoft.com/) (Community is fine) **or** Build Tools, with:
   - Workload: **Desktop development with C++**
   - Ensure **MSVC v143**, a **Windows 10/11 SDK**, and **C++ CMake tools for Windows** (bundles Ninja) are installed.
2. Install **Python 3** and put it on `PATH` — CMake uses it for the one-time `meshcodec` patch step.
3. From the repo root, double-click or run in **cmd** (not MSYS/Git Bash for these scripts):

```batch
build-release.bat
```

Debug or RelWithDebInfo:

```batch
build-debug.bat
build-relwithdbginfo.bat
```

The executable is copied to `rootDir\Starlight.exe`. The full build tree lives under `out\build\<preset>\`.  
Add `nopause` as the first argument to skip the pause at the end (`build-release.bat nopause`).

**Windows (Visual Studio IDE)**  
Open the folder as a CMake project, pick the **x64-release** (or **x64-debug**) **configure preset**, then build target **Starlight**.

**Windows packaging (ZIP + installer)**  
Requires [NSIS](https://nsis.sourceforge.io/) on `PATH` or discoverable by CPack for the installer generator:

```batch
build-and-package.bat
```

**macOS / Linux**  
Use a normal developer toolchain (Xcode command-line tools or GCC/Clang, CMake, Ninja or Make). From the repo root:

```bash
chmod +x build-release.sh
./build-release.sh
```

Output: `rootDir/Starlight` (copied from `build/src/Starlight`).

**Common pitfalls**

- **Wrong CMake on `PATH`** (Git/MSYS, devkitPro, etc.): the Windows `.bat` scripts use Visual Studio’s bundled `cmake.exe` and run `VsDevCmd` so MSVC headers and `cl.exe` resolve correctly — prefer them over raw `cmake` from an arbitrary shell.
- **Stale build directory**: if configures fail oddly, delete the matching folder under `out/build/` (or legacy `build/`) and rerun the script.
