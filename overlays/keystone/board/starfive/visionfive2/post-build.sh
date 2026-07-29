#!/bin/sh

# Copy extlinux config
mkdir -p $TARGET_DIR/boot/extlinux
cp $BR2_EXTERNAL_KEYSTONE_PATH/board/starfive/visionfive2/extlinux.conf $TARGET_DIR/boot/extlinux/extlinux.conf

# Configure network interfaces
rm -f $TARGET_DIR/etc/systemd/network/end0.network
cp $BR2_EXTERNAL_KEYSTONE_PATH/board/starfive/visionfive2/10-end0.network $TARGET_DIR/etc/systemd/network/10-end0.network
cp $BR2_EXTERNAL_KEYSTONE_PATH/board/starfive/visionfive2/20-end1.network $TARGET_DIR/etc/systemd/network/20-end1.network

# Copy sshd config to enable ssh root login, password is "starfive"
cp $BR2_EXTERNAL_KEYSTONE_PATH/board/starfive/visionfive2/sshd_config $TARGET_DIR/etc/ssh/sshd_config

# Add project-provided root SSH keys without replacing locally generated keys.
mkdir -p "$TARGET_DIR/root/.ssh"
touch "$TARGET_DIR/root/.ssh/authorized_keys"
while IFS= read -r key; do
    [ -n "$key" ] || continue
    grep -qxF "$key" "$TARGET_DIR/root/.ssh/authorized_keys" ||
        printf '%s\n' "$key" >> "$TARGET_DIR/root/.ssh/authorized_keys"
done < "$BR2_EXTERNAL_KEYSTONE_PATH/board/starfive/visionfive2/authorized_keys"
chmod 700 "$TARGET_DIR/root/.ssh"
chmod 600 "$TARGET_DIR/root/.ssh/authorized_keys"

# Copy experiment scripts and other Keystone rootfs overlay files.
if [ -d "$BR2_EXTERNAL_KEYSTONE_PATH/fs" ]; then
    cp -a "$BR2_EXTERNAL_KEYSTONE_PATH/fs/." "$TARGET_DIR/"
fi
