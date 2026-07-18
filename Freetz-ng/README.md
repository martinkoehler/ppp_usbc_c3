# Freetz-ng integration

The contents of this directory form an overlay for a Freetz-ng source tree.
They are not a standalone Freetz-ng checkout. Copy the contents of this
directory into the root of an existing Freetz-ng tree, select the required
packages with `make menuconfig`, and build the firmware with `make`.

The overlay adds:

- a static `esp32` addon with PPP configuration, boot scripts, routing hooks,
  and automatic start/stop of the MQTT collector;
- the `mqtt2sqlite` package, which subscribes to MQTT topics and stores the
  messages in SQLite; and
- the `mqtt-grafana` package, a read-only JSON CGI endpoint for querying that
  database from Grafana or another HTTP client.

## Before building

Start with a Freetz-ng checkout that supports the exact FRITZ!Box model and
installed FRITZ!OS release. Complete an ordinary Freetz-ng build for that
device first if possible; this separates general toolchain or firmware issues
from problems introduced by this overlay.

CDC ACM must be built by Freetz-ng for the selected router and kernel. Kernel
modules are **not portable between arbitrary FRITZ!Box models or kernels**.
Check the target router when diagnosing a module problem:

```sh
uname -m
uname -r
```

This overlay deliberately does not contain a prebuilt `cdc-acm.ko` or a copy
of Freetz-ng's generated `make/kernel` tree. The module is enabled with
`make kernel-menuconfig`, built by Freetz-ng, and selected for installation
through `make menuconfig`, as described below. This ensures that the module
matches the selected target's architecture, kernel release, and ABI. A stale
or mismatched module is normally reported as `Invalid module format` on the
router.

The target also needs USB host support for the port used by the ESP32-C3. On
some FRITZ!Box models the USB port must be configured for host mode in the AVM
web interface.

### Current PPP filename check

The current overlay contains the default file as
`/etc/default.esp32/esp32c3.config`, while `rc.ppp_esp` currently looks for
`/etc/default.esp32/ppp/esp32c3.conf`. These names must agree before the image
is built. Either change `DEFAULTDIR` and `CFG` in
`addon/esp32/root/etc/init.d/rc.ppp_esp`, or move/rename the default
file to the location expected by that script. For example, the script can be
changed to use:

```sh
DEFAULTDIR=/etc/default.esp32
RUNTIMEDIR=/mod/etc/ppp
CFG=$RUNTIMEDIR/esp32c3.config
```

The `ip-up` hook currently exports two comma-separated filters through the
legacy singular variable `MQTT_TOPIC`. Because the collector treats that as
one topic, change the hook to use the plural variable before building:

```sh
export MQTT_TOPICS="+/power/get,+/energycounter/get"
```

Review all site-specific values as well:

- serial device and speed in `esp32c3.config` (default `/dev/ttyACM0`,
  `115200`);
- router-side and ESP32-side PPP addresses (currently
  `192.168.178.83:192.168.178.250`);
- route installed by `ip-up` (currently `192.168.4.0/24` through
  `192.168.178.250`);
- MQTT broker, port, topics, and optional database path in `ip-up`; and
- the database path configured for `mqtt-grafana`, which must be the same as
  `MQTT_DB_PATH` used by `mqtt_to_sqlite`.

## Copy the overlay into Freetz-ng

Assume the repositories are next to each other:

```text
work/
├── freetz-ng/
└── ppp_usbc_c3/
```

From the parent directory, copy the **contents** of `Freetz-ng`, not the
directory itself:

```sh
cp -a ppp_usbc_c3/Freetz-ng/. freetz-ng/
```

Equivalently, from inside the Freetz-ng source directory:

```sh
cp -a ../ppp_usbc_c3/Freetz-ng/. .
```

The trailing `/.` is significant: after copying, paths such as
`freetz-ng/addon/static.pkg` and `freetz-ng/make/pkgs/mqtt2sqlite` must exist.
Do not end up with `freetz-ng/Freetz-ng/...`.

`addon/static.pkg` is part of the overlay and lists the `esp32` addon. If the
destination tree already has a customized `addon/static.pkg`,
merge the two addon lists instead of losing the existing entries. Running the
copy in a clean Git worktree makes overwritten and added files easy to audit:

```sh
cd freetz-ng
git status --short
git diff -- addon/static.pkg make/pkgs/mqtt2sqlite
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
options required by that Freetz-ng checkout. Then enable these overlay
packages:

- `mqtt2sqlite (MQTT -> SQLite collector)`
- `mqtt-grafana (SQLite JSON endpoint)` if the database should be queried over
  HTTP

Menu locations can change between Freetz-ng revisions. In menuconfig, press
`/` and search for the exact symbols `FREETZ_PACKAGE_MQTT2SQLITE` and
`FREETZ_PACKAGE_MQTT_GRAFANA`; the result shows the current menu path and lets
you jump to it. Enabling the packages selects their declared dependencies:

- `mqtt2sqlite`: Mosquitto client library and SQLite
- `mqtt-grafana`: SQLite

Also search for and enable the target's `pppd` package so that
`/usr/sbin/pppd` is present in the firmware. Selecting `pppd` selects the
Freetz PPP package and required PPP kernel-module symbols, including
`ppp_generic` and `ppp_async`, when the target does not already provide them.
The optional PPP CGI and `chat` utility are not required by these scripts.

Set **Level of user competence** to **Expert** if the **Kernel modules** menu
is hidden; Freetz-ng exposes that menu only in Expert or Developer mode.

In **Kernel modules**, set **Own Modules** to:

```text
cdc_acm
```

The name has no `.ko` suffix, and Freetz-ng requires `-` to be written as `_`
in this field. This tells Freetz-ng's image assembly to copy the generated
`cdc-acm.ko` into the firmware. It does not enable the driver in the Linux
kernel configuration; that is the next step.

Ensure the image also includes an `ip` utility with route support and the
module-loading tools used by the scripts (`modprobe`, with `insmod` as the
fallback). The exact symbols differ by Freetz-ng target and revision, so
verify the generated image rather than assuming the utilities are supplied by
the base firmware.

For the checked working 3272 configuration, the main selections include
`FREETZ_PACKAGE_PPPD=y`, `FREETZ_PACKAGE_PPP=y`,
`FREETZ_PACKAGE_MQTT2SQLITE=y`, and `FREETZ_PACKAGE_MQTT_GRAFANA=y`.

When `mqtt-grafana` is enabled, configure its submenu:

- **SQLite database path**: absolute path to the database written by the
  collector. Prefer persistent USB storage; `/var` is generally temporary.
- **Maximum rows per request**: hard cap for one CGI response.
- **Allowed client IP**: optional exact `REMOTE_ADDR` match for the Grafana
  host. Leave empty to allow all clients that can authenticate to the Freetz
  web server.

Save the configuration when leaving menuconfig. Freetz-ng writes it to
`.config`. It is worth confirming the important selections before the build:

```sh
grep -E 'FREETZ_PACKAGE_(MQTT2SQLITE|MQTT_GRAFANA)' .config
grep -E 'FREETZ_PACKAGE_(PPP|PPPD)=|FREETZ_MODULES_OWN=' .config
```

The static ESP32 addon is activated by `addon/static.pkg`; it does not have a
separate menuconfig entry in this overlay.

### Configure the kernel

`make menuconfig` is not sufficient by itself for CDC ACM. Freetz-ng has no
dedicated `FREETZ_MODULE_cdc_acm` selection that automatically changes the
Linux kernel configuration. After selecting and saving the router model and
firmware in `make menuconfig`, run this inside the container:

```sh
make kernel-menuconfig
```

In the kernel menu, press `/`, search for `CONFIG_USB_ACM`, and set **USB Modem
(CDC ACM) support** to `M` (module). Its usual menu path on the 2.6.32 target
is:

```text
Device Drivers
  USB support
    USB Device Class drivers
      USB Modem (CDC ACM) support = M
```

Save when leaving. Freetz-ng copies the kernel configuration back to the
selected target profile under its own `make/kernel/configs/freetz` directory.
That changed profile belongs to the Freetz-ng checkout; it is generated
configuration state and is not supplied by this overlay.

Selecting `pppd` in the main menu normally supplies the PPP module selections
needed for this serial link. In `kernel-menuconfig`, confirm that **PPP
support** and **PPP support for async serial ports** are `M` if the selected
target did not already enable them. There is no need to enable PPP compression
or MPPE solely for the ESP32 link because the supplied PPP options disable
compression.

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
modules, while the static addon installs the ESP32 files. At boot, the
`S80ppp_esp` link invokes `rc.ppp_esp`, which:

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
MQTT_DB_PATH=/var/media/ftp/storage/mqtt_messages.db \
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
- **PPP config is not seeded:** resolve the filename/path mismatch described
  under “Current PPP filename check”, then rebuild the image.
- **PPP starts but the ESP32 subnet is unreachable:** compare the negotiated
  addresses with `esp32c3.config` and check the route installed by `ip-up`.
- **Only one malformed subscription appears:** use `MQTT_TOPICS`, not
  comma-separated values in `MQTT_TOPIC`.
- **No database:** use an absolute writable `MQTT_DB_PATH`, ensure its parent
  storage is mounted before PPP comes up, and inspect collector output.
- **CGI returns 403:** the configured allowed IP does not equal the request's
  `REMOTE_ADDR`.
- **CGI returns 503:** its compiled-in database path is missing, unreadable, or
  different from the collector's path. Change menuconfig and rebuild.
