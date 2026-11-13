#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -euo pipefail

project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-RELEASE}
fip_tree="${project_root}/fip-board/x96max-plus"
fd="${project_root}/out/${target}/X96M_EFI.fd"
output_dir="${project_root}/out/${target}/fip"
work_dir="${project_root}/out/${target}/fip-work"

test -s "${fd}"
# The FD no longer has a fixed size: the eMMC flash-in build shrinks it to
# whatever the boot partition can hold (see FDF).  Sanity-bound instead.
fd_size=$(stat -c %s "${fd}")
test "${fd_size}" -gt $((0x20000))
test "${fd_size}" -le $((4*1024*1024))

mkdir -p "${output_dir}" "${work_dir}"

# The media/USB images are side effects of the u-boot.bin bootmk rule; clean
# all four products up front so make can never leave a stale wrapper behind.
rm -f \
  "${output_dir}/u-boot.bin" \
  "${output_dir}/u-boot.bin.sd.bin" \
  "${output_dir}/u-boot.bin.usb.bl2" \
  "${output_dir}/u-boot.bin.usb.tpl"

make -C "${fip_tree}" \
  BL33="${fd}" \
  O="${output_dir}" \
  TMP="${work_dir}" \
  "${output_dir}/u-boot.bin"

media_image="${output_dir}/u-boot.bin.sd.bin"
test -s "${media_image}"
test "$(od -An -tx1 -j $((0x10210)) -N 8 "${media_image}" | tr -d ' \n')" = \
  "010064aa78563412"

# Hardware boot partitions are block devices; produce a zero-padded
# sector-aligned copy for dd writes to /dev/mmcblk*boot0 (NOT used by the
# SD-card-only bring-up).
fip_size=$(stat -c %s "${media_image}")
cp "${media_image}" "${output_dir}/u-boot.bin.sd.emmc.bin"
truncate -s "$(( (fip_size + 511) / 512 * 512 ))" "${output_dir}/u-boot.bin.sd.emmc.bin"

sha256sum "${fd}" \
  "${work_dir}/bl33.bin.enc" \
  "${output_dir}/u-boot.bin" \
  "${media_image}" \
  "${output_dir}/u-boot.bin.usb.bl2" \
  "${output_dir}/u-boot.bin.usb.tpl" \
  > "${output_dir}/SHA256SUMS"

echo "==> FIP artifacts ready in ${output_dir}"
