#!/usr/bin/env bash
# Run the CTest suite headlessly with the Khronos validation layer.
#
# Usage: tools/ctest-validation.sh [BUILD_DIR] [extra ctest args...]
#
# Uses SDL's offscreen driver so no window appears. The validation layer is
# taken from the loader's default path when installed (CI, `dnf install
# vulkan-validation-layers`), otherwise from SS2_VK_LAYER_ROOT, which defaults
# to the privately staged Fedora copy on the ARM64 development host.
set -euo pipefail

build_dir=${1:-build}
[[ $# -gt 0 ]] && shift

layer_root=${SS2_VK_LAYER_ROOT:-$HOME/ss2-private/tools/fedora-aarch64/usr}
layer_json=VkLayer_khronos_validation.json

if ! compgen -G "/usr/share/vulkan/explicit_layer.d/$layer_json" >/dev/null &&
   ! compgen -G "/etc/vulkan/explicit_layer.d/$layer_json" >/dev/null; then
    if [[ -f "$layer_root/share/vulkan/explicit_layer.d/$layer_json" ]]; then
        export VK_ADD_LAYER_PATH="$layer_root/share/vulkan/explicit_layer.d${VK_ADD_LAYER_PATH:+:$VK_ADD_LAYER_PATH}"
        export LD_LIBRARY_PATH="$layer_root/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    else
        echo "ctest-validation: Khronos validation layer not found." >&2
        echo "Install vulkan-validation-layers or set SS2_VK_LAYER_ROOT." >&2
        exit 2
    fi
fi

export SDL_VIDEO_DRIVER=${SDL_VIDEO_DRIVER:-offscreen}
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-halt_on_error=1}
exec ctest --test-dir "$build_dir" -j"$(nproc)" --output-on-failure --no-tests=error "$@"
