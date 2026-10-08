#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${CRYGET_VERSION:-2026.10.08}"
output_dir="$project_dir/dist"
linux_version="${version}+debian13"
linux_package="cryget_${linux_version}_amd64.deb"
windows_package="crYGet-Setup-${version}-windows-x64.exe"

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

packages=("$linux_package" "$windows_package")
if [[ -r /etc/os-release ]]; then
    . /etc/os-release
    if [[ "${ID:-}" == debian ]]; then
        native_version="${version}+debian.${VERSION_CODENAME:-unknown}"
        CRYGET_VERSION="$native_version" OUTPUT_DIR="$output_dir" \
            bash "$project_dir/scripts/build-linux.sh"
        packages+=("cryget_${native_version}_$(dpkg --print-architecture).deb")
    fi
fi
(cd "$output_dir" && sha256sum "${packages[@]}" > SHA256SUMS)
echo "Release files: $output_dir"
