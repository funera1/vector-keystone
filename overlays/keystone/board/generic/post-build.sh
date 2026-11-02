#!/bin/sh

# Copy experiment scripts and other Keystone rootfs overlay files.
if [ -d "$BR2_EXTERNAL_KEYSTONE_PATH/fs" ]; then
    cp -a "$BR2_EXTERNAL_KEYSTONE_PATH/fs/." "$TARGET_DIR/"
fi
