# Bundled NVIDIA PhysX SDK

- Version: 5.9.1 (`version.txt`), tag `110.3-omni-and-physx-5.9.1` of
  https://github.com/NVIDIA-Omniverse/PhysX
- License: Apache-2.0, original `LICENSE.md` retained here
- Built with Visual Studio 2026 (MSVC 14.51) x64, CPU only (no GPU/CUDA projects)
- CMake switches: `NV_USE_STATIC_WINCRT=OFF`, `NV_USE_DEBUG_WINCRT=ON`,
  `PX_GENERATE_STATIC_LIBRARIES=OFF`, `PX_BUILDSNIPPETS=OFF`, `PX_BUILDPVDRUNTIME=OFF`

The CRT matches the engine (`staticruntime "off"`): `debug` uses /MDd,
`profile` and `release` use /MD.

| Engine configuration | PhysX configuration | Notes |
| --- | --- | --- |
| Debug | `lib/x64/debug` | `PX_DEBUG`, `PX_CHECKED`, PVD |
| Release | `lib/x64/profile` | `PX_PROFILE`, PVD |
| Shipping | `lib/x64/release` | no PVD |

Includes the SDK headers (`include`, with the generated `PxConfig.h` for DLL
linking), the import libraries and DLLs of PhysX, PhysXCommon, PhysXFoundation
and PhysXCooking, and the static libraries PhysXExtensions, PhysXPvdSDK,
PhysXCharacterKinematic, PhysXVehicle and PhysXTask.

Sources, snippets, PDBs, the GPU DLL and the internal static libraries that are
already linked into the DLLs are excluded.

To rebuild: configure `physx/compiler/public` with CMake
(`-G "Visual Studio 18 2026" -A x64 -DTARGET_BUILD_PLATFORM=windows
-DPHYSX_ROOT_DIR=<physx> -DPX_OUTPUT_LIB_DIR=<physx> -DPX_OUTPUT_BIN_DIR=<physx>`
plus the switches above), build `debug`, `profile` and `release`, then copy
`bin/win.x86_64.vc143.md/<config>` and `include` here.
