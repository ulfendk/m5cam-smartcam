# m5cam-smartcam

Firmware that turns an **M5Stack M5Camera** (M5CAM, including the M5CAM F fisheye) into a camera for
[SmartCam](https://github.com/ulfendk/smart-camera-home), the self-hosted camera alarm for Home Assistant.

The camera serves its OV2640 as an MJPEG stream. go2rtc (inside SmartCam) turns that into H.264 with real
wall-clock timestamps, so SmartCam records, detects and plays it like any other IP camera.

## Hardware

Tested on an M5CAM F (ESP32-D0WDQ6, 4 MB flash, 4 MB PSRAM, OV2640, fisheye lens). The pin map is M5Camera
"model B": XCLK 27, SIOD 22, SIOC 23, VSYNC 25, HREF 26, PCLK 21, RESET 15, D0–D7 32/35/34/5/39/18/36/19.

Measured over WiFi (RSSI −50):

| Frame size | Frame rate | JPEG per frame |
|---|---|---|
| 1600×1200 | ~5 fps | ~87 KB |
| 1024×768 (default) | ~8 fps | ~40 KB |
| 1280×720 | ~8 fps | ~44 KB |
| 800×600 | ~14 fps | ~28 KB |

## Flashing

Needs [PlatformIO](https://platformio.org) (`uv tool install platformio`) and access to the serial port
(on Arch: `sudo usermod -aG uucp $USER`, then log in again).

```bash
pio run -t upload --upload-port /dev/ttyUSB0
```

Back up the factory firmware first if you may want it back:
`uvx esptool --port /dev/ttyUSB0 read-flash 0 0x400000 m5cam-factory.bin`.

## WiFi

The camera only does 2.4 GHz WiFi (WPA2). There are two ways to set it up.

**Captive portal.** With no network configured, or none reachable for 30 s, the camera opens the access point
**SmartCam-xxxxxx**. Join it from a phone, pick the network and enter its password (and optionally a camera
password) in the page that opens.

**USB serial** (115200 baud, e.g. `pio device monitor`), one command per line:

```
ssid=MyNetwork
pass=secret
admin=camera-password      # optional: HTTP basic auth (user "admin") on every endpoint
reboot
```

Each command is confirmed with its length (`ok pass (17 bytes)`), so a mistyped password is easy to spot. Other
commands: `status` (settings and IP as JSON), `factory` (wipe all settings).

### Troubleshooting

The serial log says why a join fails:

| Log line | Meaning |
|---|---|
| `reason 15 (wrong password)` | the access point rejected the password. Check its length against the `ok pass` echo |
| `reason 201 (network not found …)` | SSID misspelt, or the network is 5 GHz only |
| `reason 202 (authentication refused)` / `reason 203 (association refused)` | MAC filter, client limit or WPA3-only network |

A single `reason 8` while rebooting is the camera leaving its old connection, and can be ignored. After 5 minutes
in portal mode the camera reboots and tries the saved network again.

If WiFi drops for more than 60 s, or the sensor stops delivering frames, the camera restarts itself.

### Several access points

When one network name is served by several access points, the camera joins the strongest. If its signal stays
below −75 dBm for three minutes, it scans and moves to an access point that is at least 8 dB stronger (at most every
ten minutes; the scan pauses the stream for a few seconds). `/status` shows the access point (`bssid`), `channel`
and `rssi`. Around −85 dBm and below the stream stops: move the camera or add an access point nearby.

Give the camera a fixed address with a DHCP reservation on its MAC (printed by `esptool flash-id`, and on the
router's client list as `smartcam-xxxxxx`): SmartCam addresses cameras by IP.

## Image orientation

Mounted upside down? Turn the image 180° (both flips; saved on the camera, so SmartCam needs no change):

```bash
curl "http://<cam>/control?var=vflip&val=1"
curl "http://<cam>/control?var=hmirror&val=1"
```

Or tick `vflip` and `hmirror` on the camera's page at `http://<cam>/`.

## Endpoints

| | |
|---|---|
| `http://<cam>/` | preview and settings page |
| `http://<cam>:81/stream` | MJPEG stream (`multipart/x-mixed-replace`) |
| `http://<cam>/capture` | one JPEG |
| `http://<cam>/status` | JSON: firmware, RSSI, uptime, stream fps, settings |
| `http://<cam>/control?var=<name>&val=<n>` | change a setting; saved across reboots |
| `POST http://<cam>/restart` | reboot |
| `POST http://<cam>/update` | over-the-air update, see below |

Settings: `framesize` (5 = 320×240 … 9 = 800×600, 10 = 1024×768, 11 = 1280×720, 13 = 1600×1200), `quality`
(4–63, lower is better), `maxfps`, `brightness`, `contrast`, `saturation` (−2…2), `vflip`, `hmirror`, `awb`,
`aec`, `agc`, `gainceiling`, `lenc`, `dcw`.

The camera announces itself over mDNS as `smartcam-xxxxxx.local` (the last six hex digits of its MAC).

## Updating over WiFi

```bash
pio run
curl --data-binary @.pio/build/m5cam/firmware.bin http://<cam>/update     # add -u admin:<password> if set
```

It answers `update ok, restarting` after about 10–15 s on a good link, and `/status` then shows the new
`firmware`. The image goes to the second app slot and is checked before the camera switches to it, so a broken
or interrupted upload leaves the running firmware in place. Update over a decent signal; a link at −85 dBm or
worse may drop mid-upload.

Firmware before 1.1.0 crashes on `/update` (its handler overflowed the web server's stack) and stays on the old
version, so cameras on 1.0.x need one update over USB.

## In SmartCam

Add the camera to `smartcam.yml`. `record` and `detect` use the same `exec:` source. SmartCam then feeds
detection from the record restream, so the camera serves only one stream:

```yaml
m5cam_source: &m5cam "exec:ffmpeg -hide_banner -loglevel error -use_wallclock_as_timestamps 1 -fflags nobuffer -f mpjpeg -i http://{ip}:81/stream -an -c:v libx264 -preset superfast -tune zerolatency -g 20 -pix_fmt yuv420p -rtsp_transport tcp -f rtsp {output}"

cameras:
  hallway:
    location: indoor
    ip: 192.168.1.50
    record: *m5cam
    detect: *m5cam
```

With a camera password, use `http://admin:<password>@{ip}:81/stream`, or keep the password out of the file
with `http://admin:${M5CAM_PASS}@{ip}:81/stream` and `M5CAM_PASS=…` in SmartCam's `.env`.

Add it by editing `smartcam.yml` and restarting SmartCam, or in SmartCam's settings UI, which validates the
config before saving. As with any indoor camera, add `record_at_home: false` to record only while the alarm is
armed away. Detection and alerts keep running either way.

Check it in SmartCam's log: `record:<name> started`, and 10 s H.264 segments under
`recordings/<date>/<hour>/<name>/`.

Why `exec:` rather than go2rtc's shorter `ffmpeg:http://…#video=h264`: MJPEG over HTTP carries no timestamps,
and that form lets ffmpeg assume 25 fps. Tested against go2rtc 1.9.14 (the version SmartCam ships), 28 s of
real time came out as 17.8 s of video, so recordings were slow-motion and SmartCam's frame clock drifted. With
`-use_wallclock_as_timestamps 1`, 30 s of real time records as 30 s of H.264 in SmartCam's 10 s segments.
Feeding go2rtc's native MJPEG restream into an `ffmpeg:` transcode does not work either: its H.264 output
carries timestamps 1/90000 s apart.

The transcode costs a little CPU on the SmartCam host (libx264 superfast at 1024×768).

## License

MIT
