# ESP32-C3 Freetz-ng integration

The contents of this directory form an overlay for a Freetz-ng source tree.
They are not a standalone Freetz-ng checkout. Copy the contents of this
directory into the root of an existing Freetz-ng tree, select the required
packages with `make menuconfig`, and build the firmware with `make`.

The overlay adds:

- the `esp32c3` integration package, which configures persistent USB PPP,
  routing, the FRITZ!Box MQTT collector, and persistent runtime settings from
  one menuconfig submenu;
- the `mqtt2sqlite` package, which subscribes to MQTT topics and stores the
  messages in SQLite; and
- the `mqtt-grafana` package, a read-only JSON CGI endpoint for querying that
  database from Grafana or another HTTP client.

## Copy the overlay into Freetz-ng

Assume the repositories are next to each other:

```text
work/
├── freetz-ng/
└── ppp_usbc_c3/
```

From the parent directory, copy the **contents** of `Freetz-ng`, not the
directory itself. `rsync` is convenient for repeat installations because it
reports changed files and handles metadata consistently:

```sh
rsync -av ppp_usbc_c3/Freetz-ng/ freetz-ng/
```

If `rsync` is unavailable, use:

```sh
cp -a ppp_usbc_c3/Freetz-ng/. freetz-ng/
```

The trailing slash on the rsync source, or `/.` with `cp`, is significant:
after copying, paths such as `freetz-ng/make/pkgs/esp32c3` and
`freetz-ng/make/pkgs/mqtt2sqlite` must exist. Do not end up with
`freetz-ng/Freetz-ng/...`.

Do **not** add `--delete` to this rsync command. The overlay is only a sparse
set of additions to a much larger Freetz-ng tree; a root-level `--delete`
would treat normal Freetz-ng files as absent from the overlay and remove them.
Neither `cp` nor safe overlay-style rsync can infer that a file removed in a
new overlay version should also be removed from an existing destination.
Review the overlay repository's changes when updating and remove explicitly
retired destination files by their exact paths.

### Update an existing Docker build tree

For an existing package-based installation under the documented
`freetz-docker` layout, remove the two retired overlay files before copying the
current overlay. Run these commands on the host from the mounted Git parent:

```sh
cd ~/freetz-docker/vol/git
rm -f freetz-ng/make/pkgs/esp32c3/files/root/etc/init.d/rc.ppp_esp
rm -f freetz-ng/make/pkgs/esp32c3/files/root/etc/default.esp32/esp32c3.config
rsync -av ppp_usbc_c3/Freetz-ng/ freetz-ng/
```

Do not remove `freetz-ng/.config`; it contains the image selections. The
package version bumps cause the changed packages to be staged and rebuilt.
Do not use `rsync --delete`.

When upgrading from the former static-addon version of this overlay, remove
the retired addon after copying the new package. Also remove the old `esp32`
line from `addon/static.pkg`. Inspect a top-level `rc.custom` first if it may
contain unrelated local configuration:

```sh
rm -rf freetz-ng/addon/esp32
sed -i '/^[[:space:]]*esp32[[:space:]]*$/d' freetz-ng/addon/static.pkg
```

Remove a top-level `freetz-ng/rc.custom` only after inspecting it and only if
it contains nothing except the retired ESP startup implementation. Preserve
it when it contains unrelated local startup commands.

Older overlay versions also installed this documentation as the destination
tree's root `README.md`, overwriting Freetz-ng's own tracked document. If that
file has no unrelated local edits, restore the upstream version once:

```sh
cd freetz-ng
git restore README.md
```

The overlay documentation is now copied as `README.esp32c3.md`, which does not
collide with Freetz-ng's README.

The Docker update sequence above removes `rc.ppp_esp`, the init-script name
retired after package version 1.0.0, and `esp32c3.config`, the PPP options
template retired by version 1.1.0. Safe overlay rsync cannot remove either
file automatically.

Current package versions create a fresh staging directory and no longer
generate `S80ppp_esp`.

The packaged PPP init script handles module loading, reconnects, routing, and
the collector lifecycle. Do not install the former large `rc.custom` on the
router, because it would start competing pppd and collector processes. The
new overlay does not replace `addon/static.pkg`.

Running the copy in a clean Git worktree makes overwritten and added files
easy to audit:

```sh
cd freetz-ng
git status --short
git diff -- make/pkgs/esp32c3 make/pkgs/mqtt2sqlite make/pkgs/mqtt-grafana
```

Re-copying the overlay after an update is supported, but it overwrites files
with the same names. Preserve any local configuration changes first.

## Configure the firmware

This repository was checked against the Docker-based build tree at
`~/freetz-docker/vol/git/freetz-ng`. In that setup, start the build container
from the host with:

```sh
cd ~/freetz-docker
make
```

That command builds the Docker image if necessary and opens a shell in the
container. The host directory `~/freetz-docker/vol/git` is mounted as
`/home/user/git`, so enter the Freetz-ng tree inside the container:

```sh
cd /home/user/git/freetz-ng
```

Run Freetz-ng configuration and build commands in this container. Merely
editing or inspecting the host-mounted files is possible on the host, but the
required build toolchain and libraries are supplied by the container.

### Migrate an existing `.config`

An existing Freetz-ng `.config` preserves the selected FRITZ!Box target and
all explicit package values. After copying a newer overlay, opening and saving
`make menuconfig` adds newly introduced symbols with their current defaults,
but it deliberately retains old values for symbols that already exist.

To preserve every existing value and only add missing settings, open
`make menuconfig`, ensure **esp32c3 USB PPP and MQTT integration** is enabled,
and save the configuration.

To retain the rest of the image configuration but reset all ESP32-C3
integration settings to the current overlay defaults, first make a backup and
remove only the subordinate ESP32-C3 and derived Grafana values:

```sh
cp -p .config .config.before-esp32c3-defaults

sed -i \
  -e '/^FREETZ_PACKAGE_ESP32C3_/d' \
  -e '/^# FREETZ_PACKAGE_ESP32C3_.* is not set$/d' \
  -e '/^FREETZ_PACKAGE_MQTT_GRAFANA_\(TOPIC\|DB_PATH\|MAX_ROWS\|ALLOWED_IP\)=/d' \
  .config

make menuconfig
```

The exact main selection `FREETZ_PACKAGE_ESP32C3=y` does not contain the
trailing underscore and is therefore preserved by these commands. If the old
configuration did not select the integration package, enable it in the menu.
Review its submenu and save; Kconfig then writes all removed settings using
the current defaults. Do not use `make defconfig`, because that would replace
the unrelated target and image selections as well.

Confirm the resulting integration settings before building:

```sh
grep -E '^FREETZ_PACKAGE_ESP32C3(=|_)|^FREETZ_PACKAGE_MQTT_GRAFANA_' .config
```

Run the configuration interface from the root of the Freetz-ng source tree in
the container:

```sh
make menuconfig
```

First select the correct FRITZ!Box model, firmware version, and normal image
options required by that Freetz-ng checkout. Then enable **esp32c3 USB PPP and
MQTT integration** (`FREETZ_PACKAGE_ESP32C3`) under **Packages**. Menu
locations can change between Freetz-ng revisions; press `/`, search for the
exact symbol, and use the result to jump to its current location.

The integration package automatically selects:

- **Point-to-Point** (`FREETZ_PACKAGE_PPP`);
- **pppd 2.4.7 - DEPRECATED** (`FREETZ_PACKAGE_PPPD`);
- `mqtt2sqlite`, the Mosquitto library plus the small `mosquitto_sub`
  diagnostic client, and SQLite; and
- `mqtt-grafana` and its SQLite dependency when **Include mqtt-grafana JSON
  endpoint** is enabled.

Freetz-ng's current Mosquitto package has no library-only menu choice; it
installs `libmosquitto` only when at least one client is selected. Therefore
the integration includes `mosquitto_sub`, which is also useful for testing.
This setup uses the Mosquitto broker on the FRITZ!Box, so leave **include
broker server** enabled in the Mosquitto submenu. It is enabled by the
package's default configuration. `mqtt2sqlite` connects to that local broker
at `127.0.0.1`; OpenBeken can publish to it through either the ESP32 access
point/PPP route or the FRITZ!Box WLAN.

Configure OpenBeken with the FRITZ!Box broker address that is reachable from
both WLAN paths (normally the FRITZ!Box LAN address or `fritz.box`) and put the
ESP access-point SSID ahead of the FRITZ!Box SSID in OpenBeken's own network
configuration. The Freetz side supplies the broker and both routes; WLAN
preference itself is a client-side setting.

`pppd` selects the required PPP kernel-module symbols, including `ppp_generic`
and `ppp_async`, when the target does not already provide them. The **ppp
dial-up-network** web interface, `pppd chat`, PPP CGI, and EAP-TLS options are
not required.

Configure these values in the `esp32c3` submenu as needed:

- USB serial device and baud rate;
- FRITZ!Box and ESP32-C3 PPP addresses, offered DNS address, and routed Wi-Fi
  subnet;
- MQTT broker address and port, topic filters, client ID, and reconnect
  timing;
- network-repair command and per-insert logging; and
- the persistent SQLite database path.

When Grafana support is enabled, the same submenu also exposes its exact
dashboard power topic, maximum rows, and optional allowed-client address.
mqtt-grafana receives the database path from the integration package, so the
writer and reader cannot be configured to use different paths accidentally.
The database default is:

```text
/var/media/ftp/FLASH-1/mqtt_messages.db
```

Change it if the USB volume has a different mount name. The storage must be
mounted and writable when the collector service starts.

The PPP endpoints default to `192.168.83.1` (FRITZ!Box) and `192.168.83.2`
(ESP32-C3). This dedicated point-to-point network is separate from the normal
FRITZ!Box LAN and the ESP access point subnet, avoiding ambiguous routes.
The collector subscriptions default to `+/power/get` and
`+/energycounter/get`, retaining the top-level MQTT wildcard. The separate
dashboard topic defaults to the exact topic `OBK-681/power/get`.

Set **Level of user competence** to **Expert** if the **Kernel modules** menu
is hidden; Freetz-ng exposes that menu only in Expert or Developer mode.

In **Kernel modules**, set **Own Modules** to:

```text
cdc_acm
```

The name has no `.ko` suffix, and Freetz-ng requires `-` to be written as `_`
in this field. This tells Freetz-ng's image assembly to copy the generated
`cdc-acm.ko` into the firmware. It does not enable the driver in the Linux
kernel configuration. **Own Modules and kernel configuration are independent;
selecting `cdc_acm` here does not set `CONFIG_USB_ACM`.** The separate
`make kernel-menuconfig` step below is mandatory.

Ensure the image also includes an `ip` utility with route support and the
`modprobe` module-loading utility used by the scripts. The exact symbols
differ by Freetz-ng target and revision, so verify the generated image rather
than assuming the utilities are supplied by the base firmware.

For the checked working 3272 configuration, the main selection is
`FREETZ_PACKAGE_ESP32C3=y`; its selected dependencies include
`FREETZ_PACKAGE_PPPD=y`, `FREETZ_PACKAGE_PPP=y`, and
`FREETZ_PACKAGE_MQTT2SQLITE=y`.

Save the configuration when leaving menuconfig. Freetz-ng writes it to
`.config`. It is worth confirming the important selections before the build:

```sh
grep -E 'FREETZ_PACKAGE_(ESP32C3|MQTT2SQLITE|MQTT_GRAFANA)' .config
grep -E 'FREETZ_PACKAGE_(PPP|PPPD)=|FREETZ_MODULES_OWN=' .config
```

### Configure the kernel

`make menuconfig` is not sufficient by itself for CDC ACM. Freetz-ng has no
dedicated `FREETZ_MODULE_cdc_acm` selection that automatically changes the
Linux kernel configuration, and this overlay intentionally does not patch
Freetz-ng's core module mapping. After selecting and saving the router model,
firmware, packages, and `cdc_acm` Own Modules entry in `make menuconfig`, run
this inside the container:

```sh
make kernel-menuconfig
```

In the kernel menu, press `/`, search for `CONFIG_USB_ACM`, and set **USB Modem
(CDC ACM) support** to `M` (module). The verified path for FRITZ!Box 3272 with
Linux 2.6.32.61 is:

```text
Device Drivers
  USB support
    USB Device Class drivers
      USB Modem (CDC ACM) support = M
```

Save when leaving. Freetz-ng copies the kernel configuration back to the
selected target profile under its own `make/kernel/configs/freetz` directory.
That changed profile belongs to the Freetz-ng checkout; it is generated
configuration state and is not supplied by this overlay. Repeat this step for
a fresh Freetz-ng clone or after switching to a target profile where
`CONFIG_USB_ACM` is not already `m`.

Selecting `pppd` in the main menu normally supplies the PPP module selections
needed for this serial link. In `kernel-menuconfig`, confirm that **PPP
support** and **PPP support for async serial ports** are `M` if the selected
target did not already enable them. There is no need to enable PPP compression
or MPPE solely for the ESP32 link because the supplied PPP options disable
compression.

Before building, the complete CDC ACM sequence is therefore:

1. In `make menuconfig`, set **Kernel modules → Own Modules** to `cdc_acm`.
2. In `make kernel-menuconfig`, set **USB Modem (CDC ACM) support** to `M`.
3. Save both configurations and run `make`.

## Build the firmware

Build from the Freetz-ng root inside the Docker container:

```sh
make
```

The first build downloads sources and builds a cross-toolchain and can take a
long time. Follow any Freetz-ng prompts concerning the selected vendor image.
The Docker setup runs the build as its `user` account; do not run the build as
root.

The `mqtt2sqlite` recipe deliberately resolves Mosquitto headers and libraries
at recipe execution time. This makes clean builds work after its declared
dependencies have been prepared. Its explicit error messages identify a
missing Mosquitto header/library or staged SQLite header.

After a successful build, use the output path printed by Freetz-ng. Flash the
image using the method supported for the selected FRITZ!Box and FRITZ!OS
version. Back up the router configuration and keep the vendor recovery tool or
another recovery method available. Firmware flashing is model-specific; do
not flash an image built for another device.

With the documented Docker setup, generated firmware is available on the host
under `~/freetz-docker/vol/git/freetz-ng/images`.

## Runtime operation

During the build, Freetz-ng compiles and installs the selected CDC ACM and PPP
modules, while the `esp32c3` package installs its generated defaults and
scripts. Freetz registers the selected package in `static.pkg`; during Freetz
startup, `rc.mod` invokes `/etc/init.d/rc.esp32c3`, which:

1. seeds `/mod/etc/conf/esp32c3.cfg` from menuconfig only when the writable
   file does not exist;
2. generates transient PPP options and installs the `ip-up`/`ip-down` hooks;
3. loads `cdc-acm`, `ppp_generic`, and `ppp_async`;
4. creates the `/dev/ppp` character device (`108:0`) if the old target did not
   populate it automatically; and
5. starts `mqtt_to_sqlite` independently of PPP; and
6. starts pppd with persistent reconnect options.

When PPP comes up, `ip-up` installs the route to the ESP32 SoftAP subnet.
`ip-down` deliberately leaves the collector running because OpenBeken may
still publish through FRITZ!Box WLAN. `mqtt_to_sqlite` has its own MQTT
reconnect loop, while pppd's `persist`, `maxfail 0`, and `holdoff` options
handle PPP reconnects. There is no additional polling supervisor.

All values exposed by the `esp32c3` menuconfig submenu are first-boot defaults,
not immutable firmware settings. Edit the persistent file on the router and
restart the integration to apply them:

```sh
nvi /mod/etc/conf/esp32c3.cfg
/etc/init.d/rc.esp32c3 restart
```

`/mod` is Freetz's persistent writable storage. On every boot—including the
first boot after installing a new firmware image—the init script tests for the
file and copies the image defaults only if it is absent. It never overwrites an
existing `/mod/etc/conf/esp32c3.cfg`. To adopt every newly built menuconfig
default, move or remove that file deliberately and restart the service; it
will seed a fresh copy. The runtime dashboard topic, database, maximum-row,
and allowed-IP values are read by `mqtt-grafana` for every CGI request.

Package version 1.1.1 uses the `.cfg` suffix so the Freetz configuration
editor accepts the file. During the first service start after upgrading, if
`esp32c3.cfg` does not exist but the former `esp32c3.conf` does, the init
script renames the old file to `esp32c3.cfg` before loading it. Existing
runtime values are therefore preserved. If both names exist, the `.cfg` file
takes precedence and the legacy file is left untouched for manual review.

### Migrate an existing runtime configuration

Preserving the runtime file also means that newly built menuconfig defaults do
not replace older values or add a newly introduced variable. After upgrading
from an overlay that pre-dates the separate dashboard topic, edit the existing
file and ensure it contains these independent settings:

```sh
MQTT_TOPICS='+/power/get,+/energycounter/get'
GRAFANA_TOPIC='OBK-681/power/get'
```

`MQTT_TOPICS` controls the collector and accepts MQTT wildcards.
`GRAFANA_TOPIC` controls the dashboard's exact database query and must not
contain `+` or `#`. Apply the edit and reload the page:

```sh
nvi /mod/etc/conf/esp32c3.cfg
/etc/init.d/rc.esp32c3 restart
```

Alternatively, to adopt every runtime default from a newly flashed image,
stop the service, move the persistent configuration to an unused backup name,
and start the service again:

```sh
/etc/init.d/rc.esp32c3 stop
mv /mod/etc/conf/esp32c3.cfg /mod/etc/conf/esp32c3.cfg.before-new-defaults
/etc/init.d/rc.esp32c3 start
```

On start, `rc.esp32c3` seeds a complete new
`/mod/etc/conf/esp32c3.cfg` from `/etc/default.esp32/esp32c3.cfg`. Perform
this only after the new firmware has booted, and keep the backup until any
intentional local customizations have been copied into the new file.

Do not remove the runtime file merely to perform a normal firmware update.
Remove it only when intentionally resetting every integration setting to the
defaults embedded in the new image.

Useful checks on the router are:

```sh
ls -l /dev/ttyACM0
lsmod | grep cdc_acm
ps | grep '[p]ppd'
ip addr show
ip route show
ps | grep '[m]qtt_to_sqlite'
ls -l /path/to/mqtt_messages.db*
```

Start, stop, or restart the PPP integration manually with:

```sh
/etc/init.d/rc.esp32c3 start
/etc/init.d/rc.esp32c3 stop
/etc/init.d/rc.esp32c3 restart
```

The collector accepts repeated command-line topics and environment-based
configuration. Topic precedence is repeated `-t`/`--topic`, then
`MQTT_TOPICS`, then the legacy single `MQTT_TOPIC`, and finally `#`.

```sh
MQTT_BROKER=127.0.0.1 \
MQTT_TOPICS='+/power/get,+/energycounter/get' \
MQTT_DB_PATH=/var/media/ftp/FLASH-1/mqtt_messages.db \
/usr/bin/mqtt_to_sqlite
```

`MQTT_TOPICS` may be comma- or whitespace-separated. Other controls are
documented in `make/pkgs/mqtt2sqlite/README`.

If `mqtt-grafana` is installed, test its health endpoint through the
authenticated Freetz web server on port 81:

```text
http://fritz.box:81/cgi-bin/mqtt-grafana.cgi?mode=health
```

A data query accepts `from`, `to`, `topic`, and `limit`. Timestamps may be Unix
seconds or milliseconds; response timestamps are milliseconds for Grafana:

```text
http://fritz.box:81/cgi-bin/mqtt-grafana.cgi?topic=OBK-681%2Fpower%2Fget&from=1700000000&to=1700003600&limit=2000
```

The dashboard does not contain a hard-coded device topic. On page load it
requests `mode=config` from the same CGI and uses the exact `GRAFANA_TOPIC`
from `/mod/etc/conf/esp32c3.cfg`. The endpoint also returns the runtime
maximum-row setting. Consequently, a `GRAFANA_TOPIC` edit is reflected after
the page is reloaded, independently of the wildcard collector subscriptions:

```text
http://fritz.box:81/cgi-bin/mqtt-grafana.cgi?mode=config
```

The endpoint opens SQLite read-only, relies on the Freetz port-81
authentication, and can additionally restrict callers to the configured IP.
Do not expose it directly to the Internet.

## Troubleshooting

- **No `/dev/ttyACM0`:** check USB host mode, cable/data wiring, kernel logs,
  and whether `cdc_acm` loaded. Device numbering may change if other ACM
  devices are attached.
- **pppd reports that `/dev/ppp` is missing:** verify that `ppp_generic` and
  `ppp_async` loaded. Package version 1.0.2 creates `/dev/ppp` as character
  device `108:0` before starting pppd.
- **No `cdc-acm.ko` in the image:** set `CONFIG_USB_ACM=m` with
  `make kernel-menuconfig` and add `cdc_acm` to **Kernel modules → Own
  Modules** in `make menuconfig`; both steps are required.
- **`Invalid module format`:** remove any stale manually copied module and
  rebuild it through Freetz-ng for the selected target kernel.
- **New menuconfig settings have no effect:** menuconfig provides first-boot
  defaults. Compare or replace `/mod/etc/conf/esp32c3.cfg`, then restart
  `rc.esp32c3`. This preservation is intentional so runtime edits survive a
  firmware update.
- **PPP starts but the ESP32 subnet is unreachable:** compare the negotiated
  addresses with `/mod/etc/conf/esp32c3.cfg` and check the route installed by
  `ip-up`.
- **No database:** use an absolute writable `MQTT_DB_PATH`, ensure
  `/var/media/ftp/FLASH-1` (or the configured parent) is mounted, and inspect
  collector output. Restart `rc.esp32c3` if the collector started before the
  volume was mounted.
- **CGI returns 403:** the configured allowed IP does not equal the request's
  `REMOTE_ADDR`.
- **CGI returns 503:** the database path in
  `/mod/etc/conf/esp32c3.cfg` is missing or unreadable. Correct it and retry;
  rebuilding is not necessary.
