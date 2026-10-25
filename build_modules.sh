#!/usr/bin/env bash
set -euo pipefail

# Run in WSL after configure-and-build.sh: builds the 7.0 modules and packs
# them as build/linux-70-modules.tar.gz for /usr/lib/modules on the microSD
# root (install from HarmonyOS, see README).
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Work area outside the repo: build/ (outputs), tools/aosp-mkbootimg, backups/.
project_dir="${MATEPAD_WORK_DIR:-$(cd "$repo_dir/.." && pwd)}"
source_dir="${SOURCE_DIR:-$HOME/matepad-linux-7.0}"
output_dir="${OUTPUT_DIR:-$HOME/matepad-linux-7.0-build}"
stage="$output_dir/modules-stage"
make_args=(-C "$source_dir" O="$output_dir" ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-)
# With CCACHE_DIR set (CI keeps it between builds), compile through ccache;
# both scripts must agree, or the second sees a changed compiler.
if [ -n "${CCACHE_DIR:-}" ] && command -v ccache >/dev/null; then
    make_args+=(CC="ccache aarch64-linux-gnu-gcc")
fi

make "${make_args[@]}" -j"${JOBS:-28}" modules
rm -rf "$stage"
make "${make_args[@]}" INSTALL_MOD_PATH="$stage" INSTALL_MOD_STRIP=1 modules_install
release=$(cat "$output_dir/include/config/kernel.release")
tar -C "$stage/lib/modules" -czf "$project_dir/build/linux-70-modules.tar.gz" "$release"
ls -l "$project_dir/build/linux-70-modules.tar.gz"
echo "release $release"
