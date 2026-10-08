#!/usr/bin/env bash
set -euo pipefail
umask 022

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
version="${CRYGET_VERSION:-2026.10.08}"
architecture="$(dpkg --print-architecture)"
output_dir="${OUTPUT_DIR:-$project_dir/dist}"
package_name="cryget_${version}_${architecture}.deb"

for program in make pkg-config dpkg-deb dpkg-shlibdeps desktop-file-validate; do
    if ! command -v "$program" >/dev/null 2>&1; then
        echo "Missing packaging tool: $program" >&2
        exit 1
    fi
done

make -C "$project_dir" -j2 cryget-desktop

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT
mkdir -p "$stage/debian" "$stage/package/DEBIAN" \
    "$stage/package/usr/bin" \
    "$stage/package/usr/share/applications" \
    "$stage/package/usr/share/metainfo" \
    "$stage/package/usr/share/doc/cryget/screenshots"

install -m 755 "$project_dir/cryget-desktop" "$stage/package/usr/bin/cryget-desktop"
install -m 644 "$project_dir/scripts/cryget.desktop" "$stage/package/usr/share/applications/cryget.desktop"
install -m 644 "$project_dir/scripts/cryget.metainfo.xml" "$stage/package/usr/share/metainfo/io.github.Annawu06.crYGet.metainfo.xml"
for icon_size in 16 32 48 64 128 256 512 1024; do
    icon_dir="$stage/package/usr/share/icons/hicolor/${icon_size}x${icon_size}/apps"
    mkdir -p "$icon_dir"
    icon_source="$project_dir/assets/cryget-${icon_size}.png"
    if [[ "$icon_size" == 1024 ]]; then icon_source="$project_dir/assets/cryget.png"; fi
    install -m 644 "$icon_source" "$icon_dir/cryget.png"
done
install -m 644 "$project_dir/screenshots/linux-download-queue.png" "$stage/package/usr/share/doc/cryget/screenshots/linux-download-queue.png"
install -m 644 "$project_dir/screenshots/linux-language-menu.png" "$stage/package/usr/share/doc/cryget/screenshots/linux-language-menu.png"
install -m 644 "$project_dir/third_party/acorn/LICENSE" "$stage/package/usr/share/doc/cryget/acorn.LICENSE"
for notice in /usr/share/doc/nodejs/copyright /usr/share/doc/libnode*/copyright; do
    [[ ! -f "$notice" ]] || install -m 644 "$notice" "$stage/package/usr/share/doc/cryget/$(basename "$(dirname "$notice")").copyright"
done
for notice in /usr/share/doc/libavformat*/copyright /usr/share/doc/libavcodec*/copyright /usr/share/doc/libavutil*/copyright; do
    [[ ! -f "$notice" ]] || install -m 644 "$notice" "$stage/package/usr/share/doc/cryget/$(basename "$(dirname "$notice")").copyright"
done

desktop-file-validate "$stage/package/usr/share/applications/cryget.desktop"
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net "$stage/package/usr/share/metainfo/io.github.Annawu06.crYGet.metainfo.xml"
fi

cat > "$stage/debian/control" <<'EOF'
Source: cryget
Section: net
Priority: optional
Maintainer: crYGet contributors

Package: cryget
Architecture: any
Depends: ${shlibs:Depends}
Description: Desktop YouTube downloader
EOF

depends="$(cd "$stage" && dpkg-shlibdeps -O -e./package/usr/bin/cryget-desktop)"
depends="${depends#shlibs:Depends=}"
if [[ -z "$depends" || "$depends" == 'shlibs:Depends='* ]]; then
    echo "Could not determine Linux runtime dependencies" >&2
    exit 1
fi

cat > "$stage/package/DEBIAN/control" <<EOF
Package: cryget
Version: $version
Section: net
Priority: optional
Architecture: $architecture
Maintainer: crYGet contributors
Depends: $depends
Description: Desktop YouTube downloader
 crYGet downloads YouTube videos with a graphical desktop interface.
EOF

mkdir -p "$output_dir"
dpkg-deb --root-owner-group --build "$stage/package" "$output_dir/$package_name"
echo "Linux installer: $output_dir/$package_name"
