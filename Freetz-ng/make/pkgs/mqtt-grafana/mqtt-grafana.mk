$(call PKG_INIT_BIN, 1.0.3)

$(PKG)_SOURCE:=
$(PKG)_SITE:=none

$(PKG)_DEPENDS_ON += sqlite

MQTT_GRAFANA_FILESDIR := $($(PKG)_TARGET_DIR)
MQTT_GRAFANA_BINARY   := $(MQTT_GRAFANA_FILESDIR)/mqtt-grafana.cgi
MQTT_GRAFANA_ROOTDIR  := $($(PKG)_DEST_DIR)
MQTT_GRAFANA_TARGET   := $(MQTT_GRAFANA_ROOTDIR)/usr/mww/cgi-bin/mqtt-grafana.cgi
SQLITE_INC            := $(firstword $(wildcard $(SOURCE_DIR)/sqlite-*/))

$(MQTT_GRAFANA_BINARY): $(PACKAGES_DIR)/.$(pkg)-$($(PKG)_VERSION)
	$(MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 -Wall -Wextra \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/include \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/include \
		-I$(SQLITE_INC) \
		-DDEFAULT_DB_PATH=\"$(call qstrip,$(FREETZ_PACKAGE_MQTT_GRAFANA_DB_PATH))\" \
		-DDEFAULT_MAX_ROWS=$(FREETZ_PACKAGE_MQTT_GRAFANA_MAX_ROWS) \
		-DDEFAULT_ALLOWED_IP=\"$(call qstrip,$(FREETZ_PACKAGE_MQTT_GRAFANA_ALLOWED_IP))\" \
		$(MQTT_GRAFANA_FILESDIR)/mqtt-grafana.c \
		-o $@ \
		-Wl,-rpath,/usr/lib/freetz \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib/freetz \
		-lsqlite3 -lm

$(MQTT_GRAFANA_TARGET): $(MQTT_GRAFANA_BINARY)
	@mkdir -p $(dir $@)
	cp -f $< $@
	[ "$(FREETZ_SEPARATE_AVM_UCLIBC)" != "y" ] || \
		$(PATCHELF_TARGET) --set-interpreter $(FREETZ_LIBRARY_DIR)/ld-uClibc.so.1 $@
	$(TARGET_STRIP) $@
	@test -d $(MQTT_GRAFANA_ROOTDIR)
	@test -x $@

$(pkg)-precompiled--int: $(MQTT_GRAFANA_TARGET)
	@test -d $(MQTT_GRAFANA_ROOTDIR)
	@test -x $(MQTT_GRAFANA_TARGET)

$(PKG_FINISH)
