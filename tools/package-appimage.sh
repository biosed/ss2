#!/usr/bin/env bash
# Package a built Linux tree into a portable AppImage using AppImage type2-runtime + mksquashfs.
# Usage: tools/package-appimage.sh <buildDir> <outAppImage>
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 BUILD_DIR OUT_APPIMAGE" >&2
    exit 2
fi

build_dir=$(realpath "$1")
out_appimage=$(realpath -m "$2")
repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

arch=$(uname -m)
case "$arch" in
    x86_64|amd64) runtime_arch="x86_64" ;;
    aarch64|arm64) runtime_arch="aarch64" ;;
    *)
        echo "Unsupported architecture for AppImage: $arch" >&2
        exit 2
        ;;
esac

if ! command -v mksquashfs >/dev/null 2>&1; then
    echo "mksquashfs not found (install squashfs-tools)" >&2
    exit 2
fi

work_dir=$(mktemp -d "${TMPDIR:-/tmp}/ss2-appimage.XXXXXX")
trap 'rm -rf "$work_dir"' EXIT

appdir="$work_dir/AppDir"
mkdir -p "$appdir/usr/lib"
cmake --install "$build_dir" --prefix "$appdir/usr"
cp "$repo_root/README.md" "$appdir/usr/README.md"

should_exclude_lib() {
    case "$(basename "$1")" in
        ld-linux*.so*|libc.so*|libm.so*|libdl.so*|libpthread.so*|librt.so*|libresolv.so*|libanl.so*|libutil.so*|libnsl.so*)
            return 0 ;;
        libstdc++.so*|libgcc_s.so*|libatomic.so*)
            return 0 ;;
        libvulkan.so*|libdrm*.so*|libgbm*.so*|libGL*.so*|libEGL*.so*|libglapi.so*)
            return 0 ;;
        libwayland-*.so*|libX11*.so*|libxcb*.so*|libXext.so*|libXau.so*|libXdmcp.so*|libXi.so*|libXfixes.so*|libXrender.so*|libXcursor.so*|libXrandr.so*|libxkbcommon*.so*)
            return 0 ;;
        libasound.so*|libpipewire-*.so*|libpulse*.so*|libdbus-1.so*|libsystemd.so*|libselinux.so*)
            return 0 ;;
        *)
            return 1 ;;
    esac
}

vorbis_lib=$(ldconfig -p | awk '/libvorbisfile\.so\.3 / {print $NF; exit}')
if [[ -n "$vorbis_lib" && -f "$vorbis_lib" ]]; then
    cp -L "$vorbis_lib" "$appdir/usr/lib/$(basename "$vorbis_lib")"
fi

for round in 1 2 3; do
    libs=()
    for target in "$appdir/usr/bin/ss2" "$appdir/usr/lib"/*.so*; do
        [[ -e "$target" ]] || continue
        while IFS= read -r libpath; do
            [[ -f "$libpath" ]] || continue
            should_exclude_lib "$libpath" && continue
            base=$(basename "$libpath")
            if [[ ! -e "$appdir/usr/lib/$base" ]]; then
                libs+=("$libpath")
            fi
        done < <(ldd "$target" 2>/dev/null | awk '/=> \// {print $3}')
    done
    for libpath in "${libs[@]}"; do
        cp -L -n "$libpath" "$appdir/usr/lib/$(basename "$libpath")"
    done
done

cat > "$appdir/AppRun" <<'EOF'
#!/usr/bin/env sh
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export XDG_DATA_DIRS="$HERE/usr/share:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}"
exec "$HERE/usr/bin/ss2" "$@"
EOF
chmod +x "$appdir/AppRun"

ln -sf usr/share/applications/io.github.biosed.ss2.desktop "$appdir/io.github.biosed.ss2.desktop"
ln -sf usr/share/icons/hicolor/scalable/apps/io.github.biosed.ss2.svg "$appdir/io.github.biosed.ss2.svg"
ln -sf usr/share/icons/hicolor/scalable/apps/io.github.biosed.ss2.svg "$appdir/.DirIcon"

cache_dir="${XDG_CACHE_HOME:-$HOME/.cache}/ss2-appimage"
mkdir -p "$cache_dir"
runtime_bin="$cache_dir/runtime-$runtime_arch"
if [[ ! -s "$runtime_bin" ]]; then
    runtime_url="https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$runtime_arch"
    curl -fsSL "$runtime_url" -o "$runtime_bin"
fi

mkdir -p "$(dirname "$out_appimage")"
mksquashfs "$appdir" "$work_dir/root.squashfs" -root-owned -noappend -comp zstd -quiet
cat "$runtime_bin" "$work_dir/root.squashfs" > "$out_appimage"
chmod +x "$out_appimage"
ls -lh "$out_appimage"
