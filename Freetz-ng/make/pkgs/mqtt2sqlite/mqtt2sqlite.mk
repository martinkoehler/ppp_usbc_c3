$(call PKG_INIT_BIN, 1.0.1)

$(PKG)_SOURCE :=
$(PKG)_SITE   := none

$(PKG)_DEPENDS_ON += mosquitto sqlite

# Package paths must be resolved here because $(PKG) changes while other
# package makefiles are processed.
MQTT2SQLITE_FILESDIR := $($(PKG)_TARGET_DIR)
MQTT2SQLITE_BINARY   := $(MQTT2SQLITE_FILESDIR)/mqtt_to_sqlite
MQTT2SQLITE_TARGET   := $($(PKG)_DEST_DIR)/usr/bin/mqtt_to_sqlite

# Use recursive expansion, not :=.
#
# These directories do not exist yet when Make initially parses this file
# after "make dirclean". They exist when the mqtt2sqlite recipe executes,
# because mosquitto and sqlite are declared as dependencies above.
MQTT2SQLITE_MOSQ_INC    = $(firstword $(wildcard $(SOURCE_DIR)/mosquitto-*/include))
MQTT2SQLITE_MOSQ_LIBDIR = $(firstword $(wildcard $(SOURCE_DIR)/mosquitto-*/lib))

# Build after the package's files/ directory has been copied.
$(MQTT2SQLITE_BINARY): $(PACKAGES_DIR)/.$(pkg)-$($(PKG)_VERSION)
	@test -n "$(MQTT2SQLITE_MOSQ_INC)" || { \
		echo "ERROR: Mosquitto include directory was not found."; \
		exit 1; \
	}
	@test -f "$(MQTT2SQLITE_MOSQ_INC)/mosquitto.h" || { \
		echo "ERROR: mosquitto.h was not found in $(MQTT2SQLITE_MOSQ_INC)."; \
		exit 1; \
	}
	@test -n "$(MQTT2SQLITE_MOSQ_LIBDIR)" || { \
		echo "ERROR: Mosquitto library directory was not found."; \
		exit 1; \
	}
	@test -e "$(MQTT2SQLITE_MOSQ_LIBDIR)/libmosquitto.so.1" || { \
		echo "ERROR: libmosquitto.so.1 was not found in $(MQTT2SQLITE_MOSQ_LIBDIR)."; \
		exit 1; \
	}
	@test -f "$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/include/sqlite3.h" || { \
		echo "ERROR: sqlite3.h was not found in the target staging directory."; \
		exit 1; \
	}
	$(MAKE_ENV) $(TARGET_CC) $(TARGET_CFLAGS) -std=gnu99 \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/include \
		-I$(TARGET_TOOLCHAIN_STAGING_DIR)/include \
		-I$(MQTT2SQLITE_MOSQ_INC) \
		$(MQTT2SQLITE_FILESDIR)/mqtt_to_sqlite.c \
		-o $@ \
		-Wl,-rpath,/usr/lib/freetz \
		-L$(MQTT2SQLITE_MOSQ_LIBDIR) \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib \
		-L$(TARGET_TOOLCHAIN_STAGING_DIR)/usr/lib/freetz \
		-l:libmosquitto.so.1 \
		-lsqlite3 \
		-lpthread \
		-lrt

# Install into the package root.
$(MQTT2SQLITE_TARGET): $(MQTT2SQLITE_BINARY)
	@mkdir -p $(dir $@)
	cp -f $< $@
	[ "$(FREETZ_SEPARATE_AVM_UCLIBC)" != "y" ] || \
		$(PATCHELF_TARGET) \
			--set-interpreter $(FREETZ_LIBRARY_DIR)/ld-uClibc.so.1 \
			$@
	$(TARGET_STRIP) $@

# Ensure the precompiled target builds and installs the executable.
$(pkg)-precompiled: $(MQTT2SQLITE_TARGET)

$(PKG_FINISH)
