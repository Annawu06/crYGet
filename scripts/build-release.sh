#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${CRYGET_VERSION:-2026.10.08.9}"
if [[ -d "$project_dir/../dist" ]]; then
    dist_root="$(cd "$project_dir/../dist" && pwd)"
else
    dist_root="$project_dir/dist"
fi
output_dir="${OUTPUT_DIR:-$dist_root/release-v$version}"
mkdir -p "$output_dir"
output_dir="$(cd "$output_dir" && pwd)"
linux_version="${version}+debian.forky"
linux_package="cryget_${linux_version}_amd64.deb"
windows_package="crYGet-Setup-${version}-windows-x64.exe"
portable_package="crYGet-${version}-windows-x64-portable.zip"

command -v zip >/dev/null || { echo "Missing packaging tool: zip" >&2; exit 1; }

bash "$project_dir/scripts/build-windows.sh"
docker build --build-arg "CRYGET_VERSION=$linux_version" \
    -f "$project_dir/scripts/linux.Dockerfile" \
    -t "cryget-linux-builder:$version" "$project_dir"

mkdir -p "$output_dir"
docker run --rm --user "$(id -u):$(id -g)" \
    --mount "type=bind,source=$output_dir,target=/out" \
    "cryget-linux-builder:$version" \
    cp "/opt/cryget/dist/$linux_package" "/out/$linux_package"
cp "$project_dir/build-windows/crYGet-Setup.exe" "$output_dir/$windows_package"
portable_stage="$(mktemp -d)"
trap 'rm -rf "$portable_stage"' EXIT
mkdir -p "$portable_stage/crYGet"
cp "$project_dir/build-windows/cryget-desktop.exe" "$portable_stage/crYGet/"
cp -a "$project_dir/build-windows/runtime/." "$portable_stage/crYGet/"
cp -a "$project_dir/build-windows/licenses" "$portable_stage/crYGet/"
rm -f "$output_dir/$portable_package"
(cd "$portable_stage" && zip -qr "$output_dir/$portable_package" crYGet)

packages=("$linux_package" "$windows_package" "$portable_package")
if [[ -r /etc/os-release ]]; then
    . /etc/os-release
    if [[ "${ID:-}" == debian ]]; then
        native_version="${version}+debian.${VERSION_CODENAME:-unknown}"
        if [[ "$native_version" != "$linux_version" ]]; then
            CRYGET_VERSION="$native_version" OUTPUT_DIR="$output_dir" \
                bash "$project_dir/scripts/build-linux.sh"
            packages+=("cryget_${native_version}_$(dpkg --print-architecture).deb")
        fi
    fi
fi
(cd "$output_dir" && sha256sum "${packages[@]}" > SHA256SUMS)
echo "Release files: $output_dir"
