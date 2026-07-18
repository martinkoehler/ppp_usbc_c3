# ppp_usbc_c3
ESP32C3 as AP with PPP connection via USB-C and local mqtt_broker

The ESP32-C3 opens a WLAN Access point where clients can connect to and access the local MQTT Broker.
It also exposes a simple status/config web UI and shows MQTT power telemetry on a small OLED.

## Freetz-ng integration

The [`Freetz-ng` overlay](Freetz-ng/README.md) contains files that can be
copied directly into a Freetz-ng source tree. It adds the router-side USB ACM
and PPP setup, MQTT-to-SQLite collection, and an optional JSON endpoint for
Grafana. The linked guide covers target compatibility, copying the overlay,
the Docker build environment, `make menuconfig`, package selection, `make`,
flashing, runtime checks, and troubleshooting.

## Usage
The ESP32 is connected to the host via USB. The "endpoint" on the host is usually something like /dev/ttyACM0.
The internal IP of the ESP32 is 192.168.4.1 in the 192.168.4.0/24 subnet
The ESP32 Wemos can be reached via PPP using 192.168.178.50 in a standard FRITZ Box setup.

These settings can be changed in the source code, only.

### Setup
Host <--> ESP32 ( <-- Client(s))

### Compile and flash

This is a ESP-IDF project.
Install ESP-IDF clone this repo and use
```
idf.py build
idf.py -p /dev/ttyACM0 flash
```

### PPP connection

On the host side establish the PPP connection using:
```
sudo pppd /dev/ttyACM0 file ./options.usb-esp32 nodetach
sudo ip route add 192.168.4.0/24 via 192.168.178.50 dev ppp0
```

You can now access the ESP32 Webserver via http://192.168.178.50 in order to configure the SSID, password, and Wi-Fi channel selection.
After "Save & Restart AP" clients can connect to the ESP32 using this data.

The Webserver shows the IP of connected client. Due to the `route add` command, these clients can be reached directly from the host (e.g. http://192.168.4.3)

## Web UI & OTA Update

The device runs a web UI for status, SSID/password config, and OTA updates.
It uses HTTP Basic authentication with username `admin` and the current
SoftAP password. For an intentionally open SoftAP, the password is empty;
administrative access is therefore not secure and should only be used on a
trusted, isolated network.

Basic authentication is enabled after every boot. To temporarily disable it,
press and release BOOT six times, with no more than two seconds between short
presses. The OLED then shows `WEB AUTH:OFF` and scrolls the current SoftAP SSID
and password (`<OPEN>` for an open network). Repeat the six-press gesture to
enable authentication again. This setting is deliberately not persisted;
resetting or power-cycling always restores authentication.

- Web UI (PPP side): http://192.168.178.50
- Web UI (SoftAP side): http://192.168.4.1

### OTA via Web UI
Open the web UI, select the `.bin` firmware, and click **Upload & Update**.
The device will reboot after a successful upload.

### OTA via wget (headless)
Build the firmware first (`idf.py build`). The default output is typically:

```
build/ppp_usb_c3.bin
```

Then upload it:

```sh
wget --method=POST \
  --user=admin --password='YOUR_AP_PASSWORD' \
  --header="Content-Type: application/octet-stream" \
  --header="X-OTA-Filename: ppp_usb_c3.bin" \
  --body-file=build/ppp_usb_c3.bin \
  http://192.168.178.50/ota -O -
```

Note: OTA uploads are most reliable over the SoftAP connection. If PPP OTA stalls,
use the SoftAP address (`http://192.168.4.1/ota`) instead.

## Prerequisites

- ESP-IDF installed and set up in your shell.
- A Linux host with `pppd` available (package name is often `ppp`).
- USB connection to the ESP32-C3 (usually enumerates as `/dev/ttyACM0`).

## Defaults

- SoftAP SSID: `ESP32C3-PPP-AP`
- SoftAP password: `12345678`
- SoftAP IP: `192.168.4.1`
- SoftAP channel selection: automatic (channels `1`, `6`, or `11`; channel `11` is the initial fallback)
- SoftAP max clients: `4`
- MQTT broker port: `1883`

PPP IP is configured in the options.usb-esp32 file; the web UI shows the current PPP IP/GW/NM when connected.

Automatic channel selection starts the SoftAP immediately on its saved fallback
channel. The first scan is deferred until the AP has remained unused for one
minute, so scan failures can never prevent startup or disrupt a client that
connects after boot. It checks again at most every six hours and only after no
Wi-Fi clients have been connected for five minutes. A channel change requires
a meaningfully better interference score. Automatic selection can be disabled
in the web UI; the manual channel field is shown only in manual mode and accepts
channels 1 through 11.

## MQTT Topics (OBK)

The built-in broker listens on port `1883` and exposes OBK telemetry:

- Power payload topic: `obk_wr/power/get`
- Connection state topic: `obk_wr/connected` with payload `online` or `offline`

The OLED and web UI show the latest power value. The web UI shows OBK
connection state in the MQTT status panel (auto-refreshed via AJAX). On the
OLED, OBK offline state is shown as `-` in place of the RSSI bar; online state
does not add a separate marker.

An internal subscriber (ESP-MQTT component) listens to these topics locally to
ensure LWT updates are reflected without patching the broker.

## OLED Display

The OLED display shows real-time power telemetry with WiFi signal strength indication.

### Normal View
- **Power (W):** Displays the latest OBK power value in large digits
- **Client Count:** Shows the number of connected WiFi clients
- **WiFi Signal Indicator:** A horizontal RSSI bar showing the strongest connected client
  - The bar spans about `-100 dBm` to `-45 dBm`
  - It has 56 fill steps, effectively one display pixel per dBm
  - The RSSI value is refreshed every 5 seconds
- **Connection Marker:** When OBK reports offline, `-` is shown in place of the RSSI bar. Online state does not show `+`.

### Screensaver
After ~60 seconds of idle (power ≤ 0), the display dims and shows a bouncing client count.

### Debug Screen
Press and hold the BOOT button (GPIO9) for about 1.2 seconds to toggle the
debug screen. The debug screen shows:
- Web server status (R=Running, S=Stopped)
- HTTP health status (last HTTP response code)
- OTA progress (if upload in progress)
- MQTT and AP state. `AP:R` means all local SoftAP checks pass. Other AP codes
  identify the failed check: `E` = no AP start event, `M` = WiFi mode, `N` =
  network interface down, `I` = IP unavailable, `C` = configuration mismatch,
  and `L` = SoftAP control block unavailable.

> **ESP32-C3 reset note:** GPIO9 is also the boot-mode strap. Do not hold the
> BOOT button while pressing or releasing RESET, because that starts the ROM
> download mode instead of this application. The OLED may retain its previous
> image in that mode, so an old `AP:R` can remain visible even though the
> firmware and access point are not running. Reset without BOOT held, wait for
> the application to start, and then long-press BOOT to open the debug screen.

### Web Authentication Recovery Screen

Press and release BOOT six times to toggle HTTP Basic authentication. Each
press must be short; a long press remains reserved for the debug screen. A
pause longer than two seconds resets the short-press counter.

While authentication is disabled, the OLED overrides its normal/debug view
and displays the current SoftAP SSID and password. The 128×64 display uses its
full width and wraps long values onto additional static lines, so credentials
do not scroll. Six more short presses restore authentication and the previous
OLED mode. Because this mode exposes credentials and administrative HTTP
endpoints, use it only temporarily in a trusted physical environment.

The web UI also provides **OLED Diagnostics → Toggle Debug Page**. It toggles
the same page without operating GPIO9 and can be used to distinguish a BOOT
button/GPIO issue from a display or power-related issue.

## WiFi Signal Strength Tracking

The system continuously monitors connected clients and their RSSI (Received Signal Strength Indicator) values:

- **Per-client RSSI:** Individual RSSI values are tracked for each connected client by MAC address
- **Best Signal:** The display shows a 56-step horizontal RSSI bar for the strongest-signal client (typically the power publisher)
- **Average Signal:** Average RSSI across all clients is available for diagnostics
- **Background Updates:** RSSI values are updated every 5 seconds in a dedicated task
- **Thread-Safe Access:** All RSSI data is protected by a mutex to prevent race conditions

This allows you to visually assess the WiFi link quality of connected devices directly on the OLED. Move the device, wait 5-10 seconds, and compare the bar length.

## Watchdog

The task watchdog has a 30-second timeout and is fed by a dedicated task every
5 seconds. That task also performs the single authoritative local HTTP health
check. After three consecutive failures it restarts only the web server. HTTP
health checks are suspended during OTA uploads. SoftAP clients are never used
as ping targets and failed client pings never restart the access point.

## Partition Table / OTA Requirements

OTA updates rely on the dual-app partition layout in `partitions.csv`:

- `otadata` for OTA selection
- `ota_0` and `ota_1` app slots

If you change the partition table, keep these entries (or adjust OTA logic accordingly), otherwise OTA updates will fail.

## Troubleshooting

- `pppd` fails to open `/dev/ttyACM0`: ensure your user is in the `dialout` group or run with `sudo`.
- No access to `192.168.4.x` clients from the host: confirm the `ip route add 192.168.4.0/24 ...` step.
- PPP interface name differs from `ppp0`: check `ip a` and update the route command to match.
- WiFi RSSI bar not updating: ensure the OLED is displaying and clients are actively connected.

## Routing

If you want the clients of the ESP32 to access other hosts of your network e.g. your router via the host, this works thanks to IP forwarding on the ESP32 code.

However usually your host does not forward packages, so this must be enabled.
On linux use:

```sh
sudo sysctl -w net.ipv4.ip_forward=1
```
Make IP forwarding permanent (survives reboot):
Edit /etc/sysctl.conf and ensure this line is present and not commented out:

```Code
net.ipv4.ip_forward=1
```
Then reload the settings:
```sh
sudo sysctl -p
```

## Fritz Box routing
If the ESP32 Wemos is connected to another host in the network you can set up routing at the Fritz box via

```sh
route add -net 192.168.4.0 netmask 255.255.255.0 gw 192.168.178.60
```
