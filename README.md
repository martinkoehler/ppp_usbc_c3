# ppp_usbc_c3
ESP32C3 as AP with PPP connection via USB-C and local mqtt_broker

The ESP32-C3 opens a WLAN Access point where clients can connect to and access the local MQTT Broker.

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
After "Save & Reboot" clients can connect to the ESP32 using this data.

The Webserver shows the IP of connected client. Due to the `route add` command, these clients can be reached directly from the host (e.g. http://192.168.4.3)

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
