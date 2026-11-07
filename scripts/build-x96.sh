#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
edk2_root="${project_root}/third_party/edk2"
edk2_platforms_root="${project_root}/third_party/edk2-platforms"

#
# Downstream patch sets, shared with the upstream vim3-edk2 tree.
#
edk2_patches=("${project_root}"/patches/firmware/edk2/*.patch)
edk2_platforms_patches=("${project_root}"/patches/firmware/edk2-platforms/*.patch)
edk2_stamp="${project_root}/out/.patchset-edk2.stamp"
edk2_platforms_stamp="${project_root}/out/.patchset-edk2-platforms.stamp"

target=${1:-DEBUG}
base_dtb="${project_root}/Platform/Amlogic/X96MaxPlus/DeviceTree/meson-sm1-x96-max-plus-2101.dtb"

# Submodule patch stamps use file timestamps relative to the patch files.
patchset_needs_sync () {
  local tree=$1 stamp=$2 skip=3
  shift 2
  if [[ ! -f ${stamp} ]]; then
    return 0
  fi
  for p in "$@"; do
    if [[ ${p} -nt ${stamp} ]]; then
      return 0
    fi
  done
  return 1
}

mkdir -p "${project_root}/out"

if patchset_needs_sync "${edk2_root}" "${edk2_stamp}" "${edk2_patches[@]}"; then
  echo "==> syncing EDK2 patch set (${#edk2_patches[@]} patches)"
  git -C "${edk2_root}" checkout --quiet -- .
  for patch_file in "${edk2_patches[@]}"; do
    echo "    ${patch_file##*/}"
    git -C "${edk2_root}" apply --check --ignore-space-change "${patch_file}"
    git -C "${edk2_root}" apply --ignore-space-change "${patch_file}"
  done
  touch "${edk2_stamp}"
fi

if patchset_needs_sync "${edk2_platforms_root}" "${edk2_platforms_stamp}" \
  "${edk2_platforms_patches[@]}"; then
  echo "==> syncing edk2-platforms patch set"
  git -C "${edk2_platforms_root}" checkout --quiet -- .
  for patch_file in "${edk2_platforms_patches[@]}"; do
    echo "    ${patch_file##*/}"
    git -C "${edk2_platforms_root}" apply --check --ignore-space-change "${patch_file}"
    git -C "${edk2_platforms_root}" apply --ignore-space-change "${patch_file}"
  done
  touch "${edk2_platforms_stamp}"
fi

export WORKSPACE="${edk2_root}"
export PACKAGES_PATH="${project_root}:${edk2_root}:${project_root}/third_party:${edk2_platforms_root}"
export GCC_AARCH64_PREFIX=aarch64-linux-gnu-

if [[ ! -x "${edk2_root}/BaseTools/Source/C/bin/GenFv" ]]; then
  make -C "${edk2_root}/BaseTools" -j"$(nproc)"
fi
# shellcheck disable=SC1091
set +u
source "${edk2_root}/edksetup.sh" BaseTools
set -u

build \
  -a AARCH64 \
  -t GCC \
  -b "${target}" \
  -p Platform/Amlogic/X96MaxPlus/X96MaxPlus.dsc \
  -D "X96_DTB_FILE=${base_dtb}" \
  -n "$(nproc)"

fd="${edk2_root}/Build/AmlogicX96MaxPlus-AARCH64/${target}_GCC/FV/X96M_EFI.fd"
test -s "${fd}"
mkdir -p "${project_root}/out/${target}"
cp "${fd}" "${project_root}/out/${target}/X96M_EFI.fd"
sha256sum "${project_root}/out/${target}/X96M_EFI.fd"
echo "==> OK: ${project_root}/out/${target}/X96M_EFI.fd"
