# Freetz-ng integration

The contents of this directory form an overlay for a Freetz-ng source tree.
They are not a standalone Freetz-ng checkout. Copy the contents of this
directory into the root of an existing Freetz-ng tree, select the required
packages with `make menuconfig`, and build the firmware with `make`.

The overlay adds:

- the `esp32c3` integration package, which configures USB PPP, routing, and
  automatic start/stop of the MQTT collector from one menuconfig submenu;
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

When upgrading from the former static-addon version of this overlay, remove
the retired addon after copying the new package. Also remove the old `esp32`
line from `addon/static.pkg`. Inspect a top-level `rc.custom` first if it may
contain unrelated local configuration:

```sh
rm -rf freetz-ng/addon/esp32
sed -i '/^[[:space:]]*esp32[[:space:]]*$/d' freetz-ng/addon/static.pkg
rm -f freetz-ng/rc.custom
```

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
The router-side broker is enabled by Freetz-ng's Mosquitto default but is not
used by this integration. To leave it out of the image, open the Mosquitto
submenu and clear **include broker server**; the selected client keeps the
required library in the image. mqtt2sqlite connects to the broker embedded in
the ESP32-C3.

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

When Grafana support is enabled, the same submenu also exposes its maximum
rows and optional allowed-client address. mqtt-grafana receives the database
path from the integration package, so the writer and reader cannot be
configured to use different paths accidentally. The default is:

```text
/var/media/ftp/Verbatim-STORENGO-01/mqtt_messages.db
```

Change it if the USB volume has a different mount name. The storage must be
mounted and writable before the PPP link comes up.

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
modules, while the `esp32c3` package installs its generated configuration and
scripts. At boot, the `S80ppp_esp` link invokes `rc.ppp_esp`, which:

1. creates the runtime PPP directory;
2. seeds the PPP options and installs the `ip-up`/`ip-down` hooks;
3. loads `cdc-acm`; and
4. starts `pppd` with a PID file.

When PPP comes up, `ip-up` installs the route to the ESP32 SoftAP subnet and
starts `/usr/bin/mqtt_to_sqlite`. When PPP goes down, `ip-down` stops the
collector. The PPP options use `persist`, unlimited reconnect attempts, and
LCP echo checks, so a disconnected ESP32 can be recovered without rebooting
the router.

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
/etc/init.d/rc.ppp_esp start
/etc/init.d/rc.ppp_esp stop
/etc/init.d/rc.ppp_esp restart
```

The collector accepts repeated command-line topics and environment-based
configuration. Topic precedence is repeated `-t`/`--topic`, then
`MQTT_TOPICS`, then the legacy single `MQTT_TOPIC`, and finally `#`.

```sh
MQTT_BROKER=192.168.178.250 \
MQTT_TOPICS='+/power/get,+/energycounter/get' \
MQTT_DB_PATH=/var/media/ftp/Verbatim-STORENGO-01/mqtt_messages.db \
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
http://fritz.box:81/cgi-bin/mqtt-grafana.cgi?topic=obk_wr%2Fpower%2Fget&from=1700000000&to=1700003600&limit=2000
```

The endpoint opens SQLite read-only, relies on the Freetz port-81
authentication, and can additionally restrict callers to the configured IP.
Do not expose it directly to the Internet.

## Troubleshooting

- **No `/dev/ttyACM0`:** check USB host mode, cable/data wiring, kernel logs,
  and whether `cdc_acm` loaded. Device numbering may change if other ACM
  devices are attached.
- **No `cdc-acm.ko` in the image:** set `CONFIG_USB_ACM=m` with
  `make kernel-menuconfig` and add `cdc_acm` to **Kernel modules → Own
  Modules** in `make menuconfig`; both steps are required.
- **`Invalid module format`:** remove any stale manually copied module and
  rebuild it through Freetz-ng for the selected target kernel.
- **New PPP menu settings have no effect:** the init script preserves an
  existing `/mod/etc/ppp/esp32c3.config`. Remove that runtime copy and restart
  `rc.ppp_esp` to seed the newly built defaults.
- **PPP starts but the ESP32 subnet is unreachable:** compare the negotiated
  addresses with `esp32c3.config` and check the route installed by `ip-up`.
- **No database:** use an absolute writable `MQTT_DB_PATH`, ensure its parent
  storage is mounted before PPP comes up, and inspect collector output.
- **CGI returns 403:** the configured allowed IP does not equal the request's
  `REMOTE_ADDR`.
- **CGI returns 503:** its compiled-in database path is missing, unreadable, or
  different from the collector's path. Change menuconfig and rebuild.
