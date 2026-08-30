################################################################################
#
# Rust standard library for riscv64gc-unknown-none-elf
#
################################################################################

RUST_STD_RISCV64GC_NONE_ELF_VERSION = 1.88.0
RUST_STD_RISCV64GC_NONE_ELF_SITE = https://static.rust-lang.org/dist
RUST_STD_RISCV64GC_NONE_ELF_SOURCE = rust-std-$(RUST_STD_RISCV64GC_NONE_ELF_VERSION)-riscv64gc-unknown-none-elf.tar.xz
RUST_STD_RISCV64GC_NONE_ELF_LICENSE = Apache-2.0 or MIT
RUST_STD_RISCV64GC_NONE_ELF_LICENSE_FILES = LICENSE-APACHE LICENSE-MIT

HOST_RUST_STD_RISCV64GC_NONE_ELF_DEPENDENCIES = host-rust-bin

define HOST_RUST_STD_RISCV64GC_NONE_ELF_INSTALL_CMDS
	(cd $(@D); ./install.sh --prefix=$(HOST_DIR) --disable-ldconfig)
endef

$(eval $(host-generic-package))
