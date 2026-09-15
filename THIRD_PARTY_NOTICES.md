# Third-party notices

Original project files are licensed under [GPLv3 only](LICENSE) and maintained separately from downloaded dependencies. Upstream notices apply to their respective components, not as a project-wide license declaration.

| Component | Use | Notice |
| --- | --- | --- |
| LLVM 23.1.1 runtimes | libc++, libc++abi, libunwind and compiler-rt, statically linked into native binaries by LLVM-MinGW 20260908 | [Native runtime notices](licenses/native-runtime-notices.txt) |
| MinGW-w64 runtime | Startup and compatibility code linked into native binaries by the same pinned toolchain | [Native runtime notices](licenses/native-runtime-notices.txt) |
| Inno Setup 6.5.4 | Builds the Windows installer; upstream installer and uninstaller notices remain embedded | [Upstream license at the pinned version](https://github.com/jrsoftware/issrc/blob/is-6_5_4/license.txt) |
| LLVM build tools | Compiler and linker, retained in the development dependency cache | [LLVM toolchain license](licenses/LLVM.txt) |

Native distributions include the project `LICENSE.txt` and copy `licenses/native-runtime-notices.txt` to one top-level `THIRD_PARTY_NOTICES.txt`. They do not need a `licenses/` or `docs/` directory or development tools. The combined third-party notice reproduces the common LLVM license once, followed by each runtime's complete additional license section and the complete MinGW-w64 runtime notice from the pinned archive. An import audit alone does not reveal statically linked runtime code.

The native binaries use Windows system DLLs and the simulator's existing public SimConnect interface; these dependencies are not redistributed. Inno Setup's existing copyright and website notices must remain intact in its binaries. Its pinned license does not require a separate product-documentation acknowledgment or an additional installed license file.

Exact versions, source locations and archive hashes are in `dependencies.json`. Dependency archives and their original notices remain intact under `build/deps/`; the repository does not vendor their source trees or ship the simulator runtime. Additional notices in a toolchain distribution must remain with that distribution. Recheck the combined runtime notice when the compiler/toolchain pin or linked libraries change.
