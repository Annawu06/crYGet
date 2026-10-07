#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$project_dir/build-windows"

for program in cmake ninja makensis x86_64-w64-mingw32-gcc x86_64-w64-mingw32-g++ x86_64-w64-mingw32-windres x86_64-w64-mingw32-objdump; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "Missing build tool: $program" >&2
        echo "On Debian/Ubuntu: sudo apt-get install cmake ninja-build nsis gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64 binutils-mingw-w64-x86-64" >&2
        exit 1
    fi
done

cmake -S "$project_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$project_dir/cmake/mingw-x64.cmake"
cmake --build "$build_dir" --target cryget-desktop

echo "Windows DLL imports:"
imports="$(x86_64-w64-mingw32-objdump -p "$build_dir/cryget-desktop.exe" | grep 'DLL Name:' || true)"
echo "$imports"
if printf '%s\n' "$imports" | grep -Eiq 'DLL Name: (libcurl|libgcc|libstdc\+\+|libwinpthread|libssl|libcrypto|zlib|libzstd|libbrotli)'; then
    echo "The executable still requires a non-system DLL; refusing to package an incomplete installer." >&2
    exit 1
fi

license_dir="$build_dir/licenses"
mkdir -p "$license_dir"
cp "$project_dir/third_party/quickjs/LICENSE" "$license_dir/quickjs.txt"
cp "$project_dir/third_party/acorn/LICENSE" "$license_dir/acorn.txt"
while IFS= read -r -d '' notice; do
    package="$(basename "$(dirname "$notice")")"
    cp "$notice" "$license_dir/$package.txt"
done < <(find "$project_dir/third_party" -mindepth 2 -maxdepth 2 -name copyright -print0)

makensis -V2 \
    "-DAPP_EXE=$build_dir/cryget-desktop.exe" \
    "-DSETUP_EXE=$build_dir/crYGet-Setup.exe" \
    "-DAPP_ICON=$project_dir/assets/cryget.ico" \
    "-DLICENSE_DIR=$license_dir" \
    "$project_dir/scripts/windows-installer.nsi"
echo "Built: $build_dir/cryget-desktop.exe"
echo "Installer: $build_dir/crYGet-Setup.exe"
