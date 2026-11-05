# Neptune

**Direct3D for Voile's Linux engine.** Neptune is UTM's Direct3D 11, DXGI and Direct3D 12 driver for Windows guests, which replays every call on the host. This fork adapts it to run in Wine for Linux on macOS, under [voile-lkd](https://github.com/Zagrios/voile-lkd).

## How it works

```text
Windows game
  └─ d3d11.dll, dxgi.dll, d3d12.dll ... Neptune's guest driver, in the game's process
       │  vtest socket
       ▼
     render server, on macOS ......... replays each call
       ├─ arm64:  DXMT ............... Direct3D 11 on Metal
       └─ x86_64: D3DMetal ........... Direct3D 11 and 12 on Metal, under Rosetta
```

The guest driver presents its frames through voile-wine's `nptunix` and voile-lkd's wsd window server.

## What is inside

| Folder | What it is | Upstream |
|---|---|---|
| `mesa/` | The guest driver (`src/virtio/neptune`): `d3d11.dll`, `dxgi.dll` and `d3d12.dll`, x86-64 PE, loaded in the game's process. It talks to the render server over the vtest socket and presents through voile-wine's `nptunix` and the wsd window server. | [osy/virtio-win-mesa](https://github.com/osy/virtio-win-mesa) `44d3259` |
| `virglrenderer/` | The render server (`virgl_test_server` and `virgl_render_server`), which replays the calls. Built for arm64, it renders with DXMT; built for x86_64 by [Voile](https://github.com/Zagrios/voile), it renders with Apple's D3DMetal through UTM's d3dmetal-native. | [utmapp/virglrenderer](https://github.com/utmapp/virglrenderer) `482f9d8` |

## Branches

- `upstream` holds the two upstream trees exactly as imported, one commit each. Update it with a new import commit.
- `main` is `upstream` plus our changes, one commit each, rebased on `upstream` after an import.

## Building

You need meson, ninja, and Python with Mako and PyYAML. The guest driver also needs [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), which Voile downloads (see its `sources.json`).

```sh
LLVM_MINGW=<llvm-mingw folder> scripts/build-client.sh   # build/client/src/virtio/neptune/{d3d11,dxgi,d3d12}.dll
scripts/build-server.sh                                    # build/prefix/{bin,libexec,lib}, for arm64
```

`build-server.sh` builds libepoxy 1.5.10 first. The server loads its backend at run time, for example `NPT_BACKEND=dxmt` with `NPT_D3D11_LIBRARY_PATH` and `NPT_DXGI_LIBRARY_PATH`. Voile builds both and ships them in its release: see [the Linux engine](https://github.com/Zagrios/voile#the-linux-engine) for how a launch starts them.

## Wine adapter versions

For `IDXGIAdapter::CheckInterfaceSupport`, meaningful backend versions and failed HRESULTs pass through unchanged. When a successful backend call reports `UINT64_MAX` (an unknown Windows UMD version on macOS), the Wine driver uses the existing DirectX registry `DriverVersion` for the matching adapter LUID, or the sole registered adapter. If no unambiguous version exists, the backend result is retained. This prevents the four-component version string from overflowing games such as Hunt: Showdown 1896; it does not establish game or anti-cheat compatibility.

The standalone `tests/adapter-version.c` Wine integration regression exercises the public DXGI API, decimal parsing and an unsupported interface. Build it with the same x86-64 llvm-mingw toolchain, linking `dxgi` and `dxguid`, and run it in a Wine prefix with the built Neptune DLLs and a display adapter. Exit 0 is a pass; 77 means display prerequisites are unavailable.

## License

Neptune, Mesa and virglrenderer are MIT licensed (Neptune: Copyright Turing Software LLC; Mesa and virglrenderer: their authors, see `mesa/docs/license.rst` and `virglrenderer/COPYING`), and their code keeps that license. Our changes are licensed under the GNU General Public License v3.0 (see `LICENSE`).

## Related repositories

| Repository | Role |
|---|---|
| [Obelisk](https://github.com/Zagrios/obelisk) | The Mac app: Steam, Epic Games and GOG libraries, downloads and launches |
| [Voile](https://github.com/Zagrios/voile) | The Windows runtime Obelisk installs, which ships this driver |
| [voile-wine](https://github.com/Zagrios/voile-wine) | The Wine inside Voile, for both engines |
| [voile-lkd](https://github.com/Zagrios/voile-lkd) | The Linux engine: x86-64 Linux programs in native macOS processes |
| **Neptune** | Direct3D for the Linux engine |
| [FEX](https://github.com/Zagrios/FEX) | FEX-Emu with its macOS port, which voile-lkd translates x86-64 code with |
