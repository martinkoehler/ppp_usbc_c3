# Change Log

## 2026-07-22 — Freetz runtime configuration suffix

- Renamed the persistent ESP32-C3 integration configuration to
  `/mod/etc/conf/esp32c3.cfg` so it can be edited by the Freetz configuration
  editor.
- Added automatic one-time migration from the former `esp32c3.conf` name when
  no `.cfg` file exists, preserving runtime changes across the upgrade.
- Updated the PPP route hook, Grafana endpoint, packaged default,
  documentation, and package versions for the new filename.
- Changed the default SQLite volume to the `FLASH` filesystem label and made
  collector startup wait asynchronously for configured `/var/media/ftp`
  volumes, preventing databases from being created on internal flash before
  USB automount completes.

## 2026-07-18 — Freetz-ng clean-build correction

- Corrected the `mqtt-grafana` recipe to use SQLite headers from the target
  staging directory instead of emitting a bare `-I` when the SQLite source
  directory is not present.
- Switched the local MQTT packages to Freetz-ng's standard package-level
  `precompiled` dependency pattern and removed redundant post-build tests.
- Added rebuild tracking for the configurable Grafana database path, row
  limit, and allowed client address, and bumped `mqtt-grafana` to 1.0.4.
- Clarified the intentionally manual CDC ACM configuration: `cdc_acm` in Own
  Modules installs the result, while `make kernel-menuconfig` must separately
  set `CONFIG_USB_ACM=m` for each fresh target profile.

## 2026-07-18 — Freetz-ng router integration and telemetry export

- Added a copyable Freetz-ng overlay with an ESP32 PPP addon, boot integration,
  PPP reconnect settings, route hooks, and collector lifecycle handling.
- Matched the overlay layout to the working Freetz-ng tree by removing the
  accidental extra addon/package directory level. CDC ACM is built for the
  selected target using `make kernel-menuconfig` and installed through
  Freetz-ng's **Kernel modules → Own Modules** setting; the overlay no longer
  carries a target-specific binary or generated kernel profiles.
- Extended `mqtt2sqlite` with repeated `-t`/`--topic` subscriptions,
  `MQTT_TOPICS`, legacy `MQTT_TOPIC` fallback, multi-topic reconnect
  subscriptions, and corrected clean-build dependency discovery.
- Added the configurable `mqtt-grafana` package: a read-only SQLite-to-JSON CGI
  endpoint with time/topic/row filtering, numeric payload conversion, health
  reporting, a request limit, and optional source-IP restriction.
- Documented the complete overlay copy, Docker-based menuconfig/build,
  deployment, runtime, and troubleshooting workflow, including target-specific
  kernel-module and current PPP filename compatibility checks.

## 2026-07-16 — Automatic Wi-Fi channel selection

- Enabled automatic SoftAP channel selection by default using weighted scans
  of the non-overlapping 2.4 GHz channels 1, 6, and 11.
- Added idle-only periodic rescans with a six-hour minimum interval, a
  five-minute client-free window, and hysteresis before changing channels.
- Added web controls for automatic/manual selection and live reporting of the
  active channel and last scan result. Manual channel entry is shown only when
  automatic selection is disabled.
- Made the AP start on its saved fallback before scanning, with a dedicated STA
  network interface, non-fatal scan handling, and a one-minute idle delay before
  the first scan, so scan setup cannot suppress the SoftAP during startup.

## 2026-07-16 — 128×64 OLED layout

- Updated the OLED geometry to use the full 128×64 display for the
  screensaver and authentication-recovery page while preserving the existing
  centered positions of the normal and debug views.
- Replaced scrolling AP credentials with centered, static wrapped lines. The
  full supported 32-character SSID and 64-character WPA key fit on the page.

## 2026-07-15 — AP stability, diagnostics, and recovery

This development session made the following changes:

- Added OLED SoftAP diagnostics, including AP event logging and compact health
  codes (`R`, `E`, `M`, `N`, `I`, `C`, and `L`).
- Removed ping-based SoftAP restarts. Corrected the task-watchdog timing to a
  30-second timeout with a 5-second feed period and enabled panic/reset on a
  real watchdog timeout.
- Added a web control that toggles the OLED debug page without using GPIO9.
- Made PPP dependent on an attached USB host. PPP now stays inactive when USB
  provides power only, disconnects when the host disappears, and retries when
  a host returns.
- Protected configuration, OTA, and OLED-control HTTP endpoints with Basic
  authentication using username `admin` and the current SoftAP password. The
  password is no longer embedded in the configuration page.
- Centralized the active HTTP health check in the watchdog task and exposed a
  synchronized cached result to the OLED.
- Made AP configuration updates complete, validated, and transactional. Form
  bodies are fully received, WPA credentials are checked, HTML output is
  escaped, and failed runtime or NVS updates roll back to the old settings.
- Propagated task and subsystem startup failures instead of silently starting
  with missing services.
- Added standard recovery for exhausted or version-incompatible NVS storage.
- Synchronized shared PPP address data, MQTT broker lifecycle state, SoftAP
  configuration/state, HTTP server lifecycle, OTA progress, and health data.
- Pinned ESP-IDF and managed-component versions, replaced source globbing with
  an explicit source list, corrected console defaults, removed an invalid PPP
  Kconfig symbol, and added the required 4 MB flash-size default.
- Invalidated cached client RSSI data after polling failures and prevented
  duplicate RSSI polling tasks.
- Added distinct BOOT gestures: a 1.2-second long press toggles the OLED debug
  page, while six short presses (maximum two-second gap) toggle web Basic
  authentication.
- Added an authentication-recovery OLED page. While authentication is off it
  scrolls the current SoftAP SSID and password; an open network is shown as
  `<OPEN>`. Authentication is always restored by reset or power-cycle.

Both the normal project build and a clean build generated only from
`sdkconfig.defaults` were verified with ESP-IDF 6.1.0.
