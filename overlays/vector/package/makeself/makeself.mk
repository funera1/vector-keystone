################################################################################
#
# makeself
#
################################################################################

MAKESELF_VERSION = release-2.7.1
MAKESELF_SITE = https://github.com/megastep/makeself/archive/refs/tags
MAKESELF_SOURCE = $(MAKESELF_VERSION).tar.gz
MAKESELF_LICENSE = GPL-2.0+
MAKESELF_LICENSE_FILES = COPYING

define HOST_MAKESELF_INSTALL_CMDS
	$(INSTALL) -D -m 0755 $(@D)/makeself.sh $(HOST_DIR)/bin/makeself
	$(INSTALL) -D -m 0644 $(@D)/makeself-header.sh $(HOST_DIR)/bin/makeself-header.sh
endef

$(eval $(host-generic-package))
