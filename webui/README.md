# VisionCam Pro embedded UI

The UI is a dependency-free, same-origin HTML/CSS/JavaScript application
served by the firmware. It avoids runtime frameworks, remote fonts/CDNs,
credential storage, and unbounded polling. API requests use the firmware's
HTTP-only session cookie and double-submit CSRF cookie.

## First-run Wi-Fi

With no saved network credentials, the camera opens `VisionCam-XXXXXX`
(WPA2 passphrase `visioncam`) at `http://192.168.4.1`. Sign in, open
**Settings → Connect camera to Wi-Fi**, and enter the 2.4 GHz SSID and
password. Credentials are staged only after the camera successfully joins the
network. The setup AP then closes and the camera restarts automatically to
apply them to NVS before camera startup. After reboot, use the camera's
address on the configured Wi-Fi network.

## Development and validation

```sh
cd webui
npm run check
cd ..
python3 tools/embed_web_assets/embed_web_assets.py webui/src firmware/src/web_assets
pio run
```

The first command runs JavaScript syntax validation and API-client contract
tests. The embed step gzip-compresses all assets under `webui/src`; generated
C files and the manifest are compiled into the firmware.

## Firmware API coverage

The UI consumes the endpoints registered in `firmware/src/web/web_server.cpp`:

- Authentication: session, login, logout, first-login password setup, and
  password/username changes.
- Device and diagnostics: device info, status, health, metrics, and restart.
- Camera and capture: camera status/settings, single-frame capture,
  authenticated MJPEG stream, and saved snapshots.
- Media/storage: paginated media and recordings, metadata, image view,
  recording playback/download/delete, index rebuild, storage status, and
  retention policy/preview/run.
- Motion: status and configuration.
- Integrations: MQTT configuration/test/reconnect/test-publish, Home
  Assistant discovery/status, network-storage configuration/test/queue.

AI and timelapse APIs are not presented: their handlers are intentionally
excluded from the current firmware build. FTP/WebDAV, flash-LED control,
snapshot download, and direct Wi-Fi configuration are also not registered
firmware routes.

Keep the UI route methods aligned with `webui/tests/api-contract.test.js`.
Tests do not send commands to a physical camera; hardware acceptance remains
tracked in `docs/TEST_PLAN.md`.
