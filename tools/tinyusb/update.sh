#!/usr/bin/env bash
# Copy the TinyUSB files used by the firmware into Middlewares/Third_Party/tinyusb.
# Usage (from the project root): tools/tinyusb/update.sh <tag>   e.g. 0.21.0
# Only the device stack is kept: core, FreeRTOS OSAL, CDC and HID classes, and
# the STM32 FS device driver (stm32_fsdev).
set -euo pipefail

TAG=${1:?usage: $0 <tag>}
DEST=Middlewares/Third_Party/tinyusb
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$TAG" https://github.com/hathach/tinyusb.git "$TMP/tinyusb"
SRC=$TMP/tinyusb
COMMIT=$(git -C "$SRC" rev-parse HEAD)

rm -rf "$DEST"
mkdir -p "$DEST/src/class/cdc" "$DEST/src/class/hid" "$DEST/src/portable/st"
cp "$SRC/LICENSE" "$DEST/"
cp "$SRC/src/tusb.c" "$SRC/src/tusb.h" "$SRC/src/tusb_option.h" "$DEST/src/"
cp -r "$SRC/src/common" "$SRC/src/device" "$SRC/src/osal" "$DEST/src/"
cp "$SRC"/src/class/cdc/{cdc.h,cdc_device.c,cdc_device.h} "$DEST/src/class/cdc/"
cp "$SRC"/src/class/hid/{hid.h,hid_device.c,hid_device.h} "$DEST/src/class/hid/"
cp -r "$SRC/src/portable/st/stm32_fsdev" "$DEST/src/portable/st/"
rm -f "$DEST/src/portable/st/stm32_fsdev/hcd_stm32_fsdev.c" # host mode, unused

cat > "$DEST/README.md" <<README
# TinyUSB (subset)

Copied by \`tools/tinyusb/update.sh $TAG\` from https://github.com/hathach/tinyusb
(tag $TAG, commit $COMMIT). MIT license, see LICENSE.

Only the device stack used by the firmware: core, FreeRTOS OSAL, CDC and HID
classes, STM32 FS device driver. Do not edit: run the script again to update.
The configuration of the stack is Inc/tusb_config.h.
README
echo "TinyUSB $TAG ($COMMIT) copied to $DEST"
