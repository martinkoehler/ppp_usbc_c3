# ppp_usbc_c3
ESP32C3 as AP with PPP connection via USB-C and local mqtt_broker

The ESP32-C3 opens a WLAN Access point where clients can connect to and access the local MQTT Broker.
It also exposes a simple status/config web UI and shows MQTT power telemetry on a small OLED.

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

You can now access the ESP32 Webserver via http://192.168.178.50 in order to configure the SSID and password.
After "Save & Restart AP" clients can connect to the ESP32 using this data.

The Webserver shows the IP of connected client. Due to the `route add` command, these clients can be reached directly from the host (e.g. http://192.168.4.3)

## Web UI & OTA Update

The device runs a web UI for status, SSID/password config, and OTA updates.

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
  --header="Content-Type: application/octet-stream" \
  --header="X-OTA-Filename: ppp_usb_c3.bin" \
  --body-file=build/ppp_usb_c3.bin \
  http://192.168.178.50/ota -O -
```

## Prerequisites

- ESP-IDF installed and set up in your shell.
- A Linux host with `pppd` available (package name is often `ppp`).
- USB connection to the ESP32-C3 (usually enumerates as `/dev/ttyACM0`).

## Defaults

- SoftAP SSID: `ESP32C3-PPP-AP`
- SoftAP password: `12345678`
- SoftAP IP: `192.168.4.1`
- SoftAP channel: `1`
- SoftAP max clients: `4`
- MQTT broker port: `1883`

PPP IP is configured in the options.usb-esp32 file; the web UI shows the current PPP IP/GW/NM when connected.

## MQTT Topics (OBK)

The built-in broker listens on port `1883` and exposes OBK telemetry:

- Power payload topic: `obk_wr/power/get`
- Connection state topic: `obk_wr/connected` with payload `online` or `offline`

The OLED and web UI show the latest power value. The OLED indicates connection
state with a `+` (online) or `-` (offline) and the web UI shows it in the MQTT
status panel (auto-refreshed via AJAX).

An internal subscriber (ESP-MQTT component) listens to these topics locally to
ensure LWT updates are reflected without patching the broker.

## OLED Display

Normal view shows the power value. A screensaver starts after ~60 seconds of
idle (power <= 0), dims the display, and bounces the connected-client count.


## Watchdog

A task watchdog periodically pings connected SoftAP clients. If a client stops
responding, the AP is restarted automatically.

## Partition Table / OTA Requirements

OTA updates rely on the dual-app partition layout in `partitions.csv`:

- `otadata` for OTA selection
- `ota_0` and `ota_1` app slots

If you change the partition table, keep these entries (or adjust OTA logic accordingly), otherwise OTA updates will fail.

## Troubleshooting

- `pppd` fails to open `/dev/ttyACM0`: ensure your user is in the `dialout` group or run with `sudo`.
- No access to `192.168.4.x` clients from the host: confirm the `ip route add 192.168.4.0/24 ...` step.
- PPP interface name differs from `ppp0`: check `ip a` and update the route command to match.

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
