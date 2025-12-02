$(call PKG_INIT_BIN, 1.0.0)

$(PKG)_SOURCE:=
$(PKG)_SITE:=none

$(PKG)_DEPENDS_ON += mosquitto sqlite

# --- bake concrete paths (no $(PKG) usage in recipes!) ---
MQTT2SQLITE_FILESDIR := $($(PKG)_TARGET_DIR)
MQTT2SQLITE_BINARY   := $(MQTT2SQLITE_FILESDIR)/mqtt_to_sqlite
MQTT2SQLITE_TARGET   := $($(PKG)_DEST_DIR)/usr/bin/mqtt_to_sqlite
# ---------------------------------------------------------

# compile after files/ got copied
# ---- bake concrete include paths ----
MOSQ_INC   := $(firstword $(wildcard $(SOURCE_DIR)/mosquitto-*/include))
SQLITE_INC := $(firstword $(wildcard $(SOURCE_DIR)/sqlite-*/))
MOSQ_LIBDIR := $(firstword $(wildcard $(PACKAGES_DIR)/mosquitto-*/root/usr/lib/freetz))

# ------------------------------------

$(MQTT2SQLITE_BINARY): $(PACKAGES_DIR)/.$(pkg)-$($(PKG)_VERSION)
	$(MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/include \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/include \
		-I$(MOSQ_INC) \
		-I$(SQLITE_INC) \
		$(MQTT2SQLITE_FILESDIR)/mqtt_to_sqlite.c \
		-o $@ \
		-Wl,-rpath,/usr/lib/freetz \
		-L$(MOSQ_LIBDIR) \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib/freetz \
		-l:libmosquitto.so.1 -lsqlite3 -lpthread -lrt


# install into package root (no INSTALL_FILE macro here!)
$(MQTT2SQLITE_TARGET): $(MQTT2SQLITE_BINARY)
	@mkdir -p $(dir $@)
	cp -f $< $@
	[ "$(FREETZ_SEPARATE_AVM_UCLIBC)" != "y" ] || \
		$(PATCHELF_TARGET) --set-interpreter $(FREETZ_LIBRARY_DIR)/ld-uClibc.so.1 $@
	$(TARGET_STRIP) $@

# ensure precompiled builds it
$(pkg)-precompiled--int: $(MQTT2SQLITE_TARGET)

$(PKG_FINISH)

