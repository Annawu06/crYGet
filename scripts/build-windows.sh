#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$project_dir/build-windows"
runtime_dir="$build_dir/runtime"
if [[ -z "${NODE_ROOT:-}" || ( ! -f "${NODE_ROOT}/include/node.h" && ! -f "${NODE_ROOT}/include/node/node.h" ) || ! -d "${NODE_ROOT}/bin" ]]; then
    echo "Set NODE_ROOT to a Node/V8 embedding SDK with include/, lib/, and bin/ directories for this compiler." >&2
    exit 1
fi

for program in cmake ninja makensis x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres x86_64-w64-mingw32-objdump; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "Missing build tool: $program" >&2
        echo "On Debian/Ubuntu: sudo apt-get install cmake ninja-build nsis gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64" >&2
        exit 1
    fi
done

cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DNODE_ROOT="$NODE_ROOT" \
    -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/mingw-x64.cmake"
cmake --build "$build_dir" --target cryget-desktop

echo "Windows DLL imports:"
imports="$(x86_64-w64-mingw32-objdump -p "$build_dir/cryget-desktop.exe" | grep 'DLL Name:' || true)"
echo "$imports"
mkdir -p "$runtime_dir"
pending=("$build_dir/cryget-desktop.exe")
declare -A visited=()
while (("${#pending[@]}")); do
    binary="${pending[0]}"
    pending=("${pending[@]:1}")
    while IFS= read -r dll; do
        name="${dll,,}"
        case "$name" in
            kernel32.dll|user32.dll|gdi32.dll|shell32.dll|ole32.dll|oleaut32.dll|windowscodecs.dll|winhttp.dll|comdlg32.dll|advapi32.dll|comctl32.dll|msvcrt.dll|ucrtbase.dll|ntdll.dll|ws2_32.dll|bcrypt.dll|secur32.dll|iphlpapi.dll|shlwapi.dll|crypt32.dll|version.dll|userenv.dll|winmm.dll|imm32.dll|setupapi.dll|dbghelp.dll|psapi.dll|rpcrt4.dll|normaliz.dll|dnsapi.dll|ncrypt.dll|mswsock.dll|wldap32.dll|netapi32.dll|powrprof.dll|dwmapi.dll|api-ms-win-*.dll|ext-ms-*.dll) continue ;;
            avcodec*.dll|avformat*.dll|avutil*.dll|swresample*.dll|swscale*.dll|postproc*.dll)
                echo "FFmpeg dependency found in $binary: $dll" >&2
                exit 1 ;;
        esac
        if [[ -n "${visited[$name]:-}" ]]; then continue; fi
        visited[$name]=1
        source="$(find "$NODE_ROOT/bin" -maxdepth 1 -type f -iname "$dll" -print -quit)"
        if [[ -z "$source" ]]; then
            echo "Missing runtime DLL required by $binary: $dll" >&2
            exit 1
        fi
        cp "$source" "$runtime_dir/"
        pending+=("$source")
    done < <(x86_64-w64-mingw32-objdump -p "$binary" | sed -n 's/^[[:space:]]*DLL Name: //p')
done
for runtime_data in "$NODE_ROOT/icudtl.dat" "$NODE_ROOT/bin/icudtl.dat"; do
    if [[ -f "$runtime_data" ]]; then cp "$runtime_data" "$runtime_dir/icudtl.dat"; break; fi
done
license_dir="$build_dir/licenses"
mkdir -p "$license_dir"
cp "$project_dir/third_party/acorn/LICENSE" "$license_dir/acorn.txt"
if [[ -f "$NODE_ROOT/LICENSE" ]]; then
    cp "$NODE_ROOT/LICENSE" "$license_dir/node.MIT"
elif [[ -f "$NODE_ROOT/share/licenses/nodejs/LICENSE" ]]; then
    cp "$NODE_ROOT/share/licenses/nodejs/LICENSE" "$license_dir/node.MIT"
elif [[ -f "$NODE_ROOT/share/doc/node/copyright" ]]; then
    cp "$NODE_ROOT/share/doc/node/copyright" "$license_dir/node.MIT"
else
    echo "NODE_ROOT must provide Node.js and V8 license notices." >&2
    exit 1
fi
v8_notice="$(find "$NODE_ROOT" -maxdepth 6 -type f \( -path '*/deps/v8/LICENSE' -o -path '*/v8/LICENSE' -o -name 'LICENSE.v8' \) -print -quit)"
if [[ -n "$v8_notice" ]]; then cp "$v8_notice" "$license_dir/v8.BSD"; else
    echo "NODE_ROOT must include V8's BSD license notice." >&2
    exit 1
fi
if [[ -d "$NODE_ROOT/share/licenses" ]]; then
    mkdir -p "$license_dir/node-runtime"
    cp -a "$NODE_ROOT/share/licenses/." "$license_dir/node-runtime/"
fi
while IFS= read -r -d '' notice; do
    package="$(basename "$(dirname "$notice")")"
    cp "$notice" "$license_dir/$package.txt"
done < <(find "$project_dir/third_party" -mindepth 2 -maxdepth 2 -name copyright -print0)

makensis -V2 \
    "-DAPP_EXE=$build_dir/cryget-desktop.exe" \
    "-DSETUP_EXE=$build_dir/crYGet-Setup.exe" \
    "-DAPP_ICON=$project_dir/assets/cryget.ico" \
    "-DLICENSE_DIR=$license_dir" \
    "-DRUNTIME_DIR=$runtime_dir" \
    "$project_dir/scripts/windows-installer.nsi"
echo "Built: $build_dir/cryget-desktop.exe"
echo "Installer: $build_dir/crYGet-Setup.exe"
