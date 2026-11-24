# VisionCam Pro — Tutorial Video Guide

A short explainer series about the VisionCam Pro ESP32-CAM firmware: seven
chapter videos plus one combined video, about **9½ minutes** in total (plus a ~10 s end screen). Each
chapter is a narration track with Manim motion graphics synced to it:
diagrams, animated charts and step pipelines, built with the
`youtube-video-creator-skill` pipeline.

**Revision 2 (2026-11-23):** adds the RTSP server, Home Assistant MQTT
discovery with two-way commands, Home Assistant live view through go2rtc,
the media worker for playback and downloads, the PSRAM memory layout, and
the RTSP Copy-URL flow. The new features are folded into the existing
chapters, so the series still has seven parts.

| # | File | Chapter | Paras | Words | Est. length |
|---|---|---|---|---|---|
| 0 | `narration/intro.txt` | Intro: what VisionCam Pro is | 6 | 140 | ~0:56 |
| 1 | `narration/ch1.txt` | Hardware & firmware foundation | 8 | 190 | ~1:16 |
| 2 | `narration/ch2.txt` | Architecture: the frame manager | 9 | 227 | ~1:30 |
| 3 | `narration/ch3.txt` | Video path: live view, RTSP, recording | 9 | 251 | ~1:40 |
| 4 | `narration/ch4.txt` | Storage, motion & Home Assistant | 9 | 249 | ~1:39 |
| 5 | `narration/ch5.txt` | Setup & security | 9 | 192 | ~1:16 |
| 6 | `narration/end.txt` | Wrap-up & call to action | 7 | 146 | ~0:58 |
| | | **Total** | 57 | ~1395 | **~9:18** |

Estimates assume 150 words per minute. The real timing comes from your
recordings: every visual beat is scheduled from the actual mp3 length.

---

## 1. Recording the narration

1. **One mp3 per txt file, same base name:** `intro.mp3`, `ch1.mp3` …
   `end.mp3`. Put them in `tutorial/narration/` next to the txt files.
2. **Read the text exactly as written.** Each recording is checked against
   its script with speech recognition, and anything below a 93% word match
   gets flagged. If you want to change a sentence, edit the txt file first,
   then record it.
3. **Pause briefly at each blank line.** Paragraphs are the unit of sync: a
   visual can only change at a paragraph boundary.
4. Numbers are already spelled out as spoken ("six forty by four eighty",
   "eighty three percent"). Say them that way. Say "RTSP" as letters,
   "MJPEG" as "M-JPEG", and "go2rtc" as "go-to-R-T-C".
5. Use a steady pace with no music bed; music can be added after the final
   mix. Any sample rate is fine (44.1 or 48 kHz mono or stereo).
6. If you already recorded the first version, re-record all seven files:
   every script changed (in ch1 only paragraph 6, the flash figure).

---

## 2. Visual style

- 1920×1080 at 30 fps, dark theme. Brand colours: VisionCam blue `#4f9cf9`
  accent on a near-black `#0f1115` background, matching the web UI. Home
  Assistant elements use HA blue `#18bcf2` so viewers can tell them apart.
- Persistent header: chapter badge (`INTRO`, `CH 01` …), chapter title and
  the "VisionCam Pro" brand, above a thin rule.
- Animations are staggered, never popped in. Each entrance takes up to about
  a third of its beat and the rest is a hold. There is no progress bar.
- Diagrams use DrawCMS specs in `.drawcms/`. A diagram enters once, then
  following beats walk through it by highlighting nodes and edges while the
  rest dims. "Pulse" means a small dot travelling along an edge to show
  data or a command moving.
- Charts are a new `chart` beat type. Bars grow from zero with a lagged
  stagger, then the value labels count up. A `stack` variant draws the flash
  partition map left to right.
- **No real credentials on screen.** Every URL or password shown uses `••••`
  or the placeholder `PASSWORD`.

---

## 3. Beat plan

`paras` = the paragraph range `[start, end)` of the chapter's txt file
(0-based, blank-line separated). The ranges tile each script exactly.

### Intro — "VisionCam Pro"

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `title` | Eyebrow "ESP32-CAM FIRMWARE", title "VisionCam Pro", sub "Private · Local-first · No cloud required". Staggered fade-up, then the rule draws. |
| [1,2) | `grid` | Six cards with icons pop in one by one: ◉ Live view, ⇄ RTSP, ▧ Snapshots, ▶ Recording, ◎ Motion, ⌂ Home Assistant. |
| [2,3) | `architecture` **ha-device** | Mock Home Assistant device card in HA blue: "VisionCam Pro" with entity rows (Motion, Recording, Snapshot button, Flash switch) and a small live-camera tile. The rows slide in, then the camera tile starts animating. |
| [3,4) | `architecture` **local-first** | Camera node at the centre feeding SD card and Browser; Internet, Cloud and Home Assistant/MQTT nodes drawn faded, then crossed out with a red ✕ while the camera→SD and camera→browser links stay lit. |
| [4,6) | `bullets` | "In this series": Hardware → Architecture → Video path & RTSP → Storage, motion & Home Assistant → Setup & security, revealed across the beat. |

### Ch 01 — Hardware & firmware foundation

| paras | beat | visual & animation |
|---|---|---|
| [0,2) | `architecture` **board** | Board block diagram: ESP32 core with OV2640, PSRAM 4 MB, microSD, flash LED (GPIO4) and the ESP32-CAM-MB USB bridge. Nodes pop in, then the USB link highlights on paragraph 1. |
| [2,3) | `grid` | Toolchain cards: PlatformIO · ESP-IDF 6.1 · no Arduino · pinned versions. |
| [3,5) | `chart` (stack) | **Flash partition map (4 MB):** a horizontal bar drawn left to right: nvs 92 KB · ota_0 1.5 MB · ota_1 1.5 MB · otadata 8 KB · phy 4 KB · storage 884 KB. Labels fade in under each block; ota_0/ota_1 pulse on paragraph 3 and nvs/storage on paragraph 4. |
| [5,6) | `stat` | "83%" counts up from 0, caption "of a 1.5 MB app slot used · all features". A ring gauge fills to 82.6%. |
| [6,7) | `statement` | "SD card in 1-bit mode", sub "keeps the shared flash-LED pin out of the data path". |
| [7,8) | `pipeline` | Boot sequence: PSRAM check → Camera init → SD mount → SD self-test → Wi-Fi → Web server. Boxes pop in and arrows draw in order. |

### Ch 02 — Architecture: the frame manager

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `chart` (bars) | **Task priorities** (horizontal bars grow): Capture 6 · Recorder 5 · Live stream 4 · RTSP 4 · Media worker 4 · Motion 4 · Event bus 4 · Network 4 · Monitor 3 · MQTT 1 · Serial console 1. |
| [1,2) | `architecture` **frame-fanout** | Camera → Capture task → **Frame manager** (accent) → subscribers. Only the camera→frame-manager edge is lit: "sole owner of camera buffers". |
| [2,4) | same diagram | Highlight the fan-out edges to Live view, **RTSP viewers**, Recorder, Motion and Snapshot. Edge labels say "lease → copy → release". |
| [4,5) | `pipeline` | Drop-oldest queue: four frame tiles slide into a two-slot queue, and when frame 3 arrives the oldest tile is pushed out and fades away. Caption "a slow viewer never stalls capture". |
| [5,6) | `architecture` **frame-fanout** (storage view) | Highlight Recorder and Snapshot → **Storage manager** → SD card. A tag "media-id, not paths" appears on the edge. |
| [6,7) | `architecture` **event-bus** | Motion → Event bus → Recorder, Web UI, MQTT. A pulse travels along each edge in turn. |
| [7,8) | `grid` (2 columns) | **Where memory lives.** Left card "Internal RAM (small)": camera DMA · SD card DMA · task stacks. Right card "PSRAM 4 MB": frames · Wi-Fi & network buffers · RTSP / recorder copies · MQTT queue. The Wi-Fi item slides from left to right. |
| [8,9) | `statement` | "Every queue is bounded", sub "degrade safely, never crash". |

### Ch 03 — Video path: live view, RTSP, recording

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `architecture` **video-path** | Frame manager → Live-stream worker → Browser (MJPEG). The HTTP server node stays lit to show it is still free for other requests. |
| [1,2) | `architecture` **rtsp-path** | Frame manager → RTSP server → three clients fanning out: VLC, NVR, Home Assistant. Label "MJPEG over RTP · TCP". The RTP edge pulses. |
| [2,3) | `grid` | Four RTSP fact cards: 🔒 Digest auth · 👤 own username/password · 3 viewers max · "just another subscriber". |
| [3,4) | `chart` (bars) | **Measured on the test board:** RTSP alone 9.9 fps (60 s) · RTSP with browser live view 12.4 fps · browser live view at the same time 11.7 fps. Caption "640×480 · 0 decode errors". |
| [4,5) | `architecture` **video-path** | Highlight Frame manager → Snapshot → Storage → SD. |
| [5,6) | `architecture` **avi-layout** | AVI file drawn as stacked blocks: RIFF header (hdrl · avih · strh · strf) → movi (00dc frame chunks ×N) → idx1 index. The frame chunks fill in one by one. |
| [6,7) | `pipeline` | `.tmp` → close → rebuild header (real count + fps) → write idx1 → rename `.avi`, then a VLC icon fades in at the end: "opens anywhere". |
| [7,8) | `architecture` **video-path** | Highlight SD → **Media worker** → Browser playback/download. Labels "own task" and "paced to fps". The HTTP server node stays green. |
| [8,9) | `chart` (bars) + `stat` | **Test clip:** 16 s · 200 frames · 12.5 fps · 0 decode errors. Counters roll up together. |

### Ch 04 — Storage, motion & Home Assistant

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `code` | Folder tree: `/sdcard/recording/manual/2026/11/23/rec-00000003.avi`, `/sdcard/system/index/recording-index.jsonl`. Lines type in. |
| [1,2) | `chart` (bars) | **Retention defaults (days):** Manual 30 · Motion 7 · Detection 30 · Timelapse 90, plus a horizontal "85% max used" threshold line drawn across. |
| [2,3) | `pipeline` | JPEG frame → decode → 96×96 grayscale → block SAD vs previous sample → "motion started". A small 4×4 grid of blocks flashes the changed ones. |
| [3,4) | `architecture` **ha-integration** | Event bus → MQTT worker → Broker → Home Assistant (HA blue). Discovery configs pulse to HA as eight small tiles. |
| [4,5) | `grid` (4×2) | The **eight entities** pop in: Motion · Recording · Camera · SD card · Storage used (%) · Wi-Fi signal (dBm) · Take snapshot (button) · Flash LED (switch). |
| [5,6) | `architecture` **ha-commands** | Two-way: HA → `cmd/snapshot` → MQTT worker → Snapshot (media count +1); HA → `cmd/flash_led` ON/OFF → flash LED lights up, then the state pulses back to HA. |
| [6,7) | `architecture` **ha-live** | Camera RTSP (MJPEG) → go2rtc (inside HA) → "ffmpeg → H.264" → WebRTC → browser. The MJPEG→H.264 box is highlighted. A faded side path "MJPEG over WebRTC ✕" shows why the conversion is needed. |
| [7,8) | `architecture` **ha-integration** | Broker node blinks offline; Capture and Recorder stay solid green and unaffected. Tag "priority 1 · bounded queue · back-off". |
| [8,9) | `grid` | Two dimmed cards, "On-device AI" and "Timelapse", each with a "roadmap · compiled out" tag. |

### Ch 05 — Setup & security

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `title` | Eyebrow "CHAPTER 5", title "Setup & security". |
| [1,2) | `pipeline` | No saved network → Setup AP "VisionCam-XXXXXX" → join → setup page → enter Wi-Fi → reboot → joins your network. |
| [2,4) | `code` | Serial line `wifi⇥<ssid>⇥••••••••` → reply `vc_console: OK wifi credentials saved (ssid 10 B, pass 8 B) - rebooting to join`. Only lengths appear. |
| [4,5) | `statement` | "First sign-in: change the default password". |
| [5,6) | `architecture` **auth** | Browser → Login → PBKDF2 (salted) → Session (RAM, HttpOnly cookie) + CSRF token → protected API. Edges light up in order. |
| [6,7) | `grid` | Lock icons on Stream · Snapshots · Recordings · Settings, plus a fifth card "RTSP · own login · hash only". |
| [7,8) | `code` | Mock of the Copy dialog: "RTSP password ••••••••" → "Generated URL `rtsp://homeassistant:••••@<camera-ip>:554/stream#video=h264`" → "Copy URL". The URL types itself in as the dots appear. |
| [8,9) | `statement` | "Local network only", sub "private by default — never expose it to the internet". |

### End — wrap-up and call to action

A standard YouTube outro: recap, then like / subscribe / comment prompts, each
with its own short beat so the animation lands on the spoken word.

| paras | beat | visual & animation |
|---|---|---|
| [0,1) | `statement` | "VisionCam Pro", sub "keeps working when everything else is offline". |
| [1,2) | `bullets` | Hardware ✓ · Frame manager ✓ · Video path & RTSP ✓ · Home Assistant ✓ · Secure setup ✓. |
| [2,3) | `grid` | Three cards: "MIT · free & open source", "⬇ Firmware download", "📖 Flashing guide", plus an arrow pointing down with "links in the description". |
| [3,4) | `statement` | 👍 A thumbs-up icon pops in with a gentle bounce, then "Like the video" and the sub "it helps the channel". |
| [4,5) | `statement` | A red **SUBSCRIBE** button scales in, then a 🔔 bell swings once with a "notifications on" tag. Hold for the whole sentence. |
| [5,6) | `statement` | 💬 A speech bubble types in "Questions? Feature ideas?" with the sub "comment below — we read every one". |
| [6,7) | `outro` | "Thanks for watching" with "see you in the next video". The logo fades in. Keep a ~10 s end-screen hold after the narration ends, for YouTube's end-screen elements (subscribe button and next-video card); then the 0.6 s fade tail. |

---

## 4. Diagrams (DrawCMS specs, built in phase 2)

| Spec | Used in | Nodes | Key edges |
|---|---|---|---|
| `ha-device` | Intro | ha-card, motion, recording, snapshot-btn, flash-switch, cam-tile | card containment (mock UI, no edges) |
| `local-first` | Intro | camera, sd, browser, internet, cloud, ha-mqtt | camera>sd, camera>browser (lit); internet/cloud/ha-mqtt dashed |
| `board` | Ch1 | esp32, ov2640, psram, sdcard, flashled, usb-mb | esp32>ov2640, esp32>psram, esp32>sdcard, usb-mb>esp32 |
| `frame-fanout` | Ch2 | camera, capture, frame-mgr, live, rtsp, recorder, motion, snapshot, storage, sd | capture>frame-mgr, frame-mgr>{live,rtsp,recorder,motion,snapshot}, {recorder,snapshot}>storage, storage>sd |
| `event-bus` | Ch2 | motion, bus, recorder, webui, mqtt | motion>bus, bus>{recorder,webui,mqtt} |
| `video-path` | Ch3 | frame-mgr, live-worker, httpd, browser, snapshot, storage, sd, media-worker | frame-mgr>live-worker>browser, frame-mgr>snapshot>storage>sd, sd>media-worker>browser |
| `rtsp-path` | Ch3 | frame-mgr, rtsp-server, vlc, nvr, home-assistant | frame-mgr>rtsp-server, rtsp-server>{vlc,nvr,home-assistant} |
| `avi-layout` | Ch3 | riff, hdrl, movi, chunks, idx1 | riff>hdrl>movi>idx1 |
| `ha-integration` | Ch4 | bus, mqtt, broker, ha, capture, recorder | bus>mqtt>broker>ha |
| `ha-commands` | Ch4 | ha, broker, mqtt, snapshot, flash-led | ha>broker>mqtt, mqtt>{snapshot,flash-led}, mqtt>ha (state) |
| `ha-live` | Ch4 | camera-rtsp, go2rtc, transcode, webrtc, browser | camera-rtsp>go2rtc>transcode>webrtc>browser |
| `auth` | Ch5 | browser, login, pbkdf2, session, csrf, api | browser>login>pbkdf2>session>api, csrf>api |

## 5. Charts (new `chart` beat, added in phase 2)

All values are real: measured on the test board or taken from the firmware
source.

| Chart | Data | Source |
|---|---|---|
| Flash partition map | nvs 92 KB, ota_0 1536 KB, ota_1 1536 KB, otadata 8 KB, phy 4 KB, storage 884 KB | `partitions.csv` |
| App slot usage | 82.6% of 1.5 MB (all features incl. RTSP) | firmware build output 2026-11-23 |
| Task priorities | capture 6, recorder 5, stream/RTSP/media/motion/eventbus/network 4, monitor 3, MQTT/console 1 | `firmware/include/tasks.h` |
| RTSP throughput | 9.9 fps alone (593 frames / 60 s); 12.4 fps RTSP + 11.7 fps browser live view concurrently; 0 decode errors | hardware test 2026-11-23 (`docs/HARDWARE.md` §7.2) |
| Test clip | 16 s, 200 frames, 12.5 fps, 0 decode errors | ffprobe/ffmpeg on a device recording |
| Retention defaults | 30 / 7 / 30 / 90 days, 85% cap | `storage_manager.h` RetentionPolicy |

---

## 6. Building the video

The pipeline lives in `tutorial/video/` (Manim Community 0.19, pinned in
`requirements-video.txt`). Run everything from `tutorial/`:

```bash
uv venv .venv && uv pip install --python .venv/bin/python -r requirements-video.txt
.venv/bin/python video/align.py              # paragraph timings from the mp3s (whisper)
.venv/bin/python video/build.py schedule     # check the beat ranges tile every script
.venv/bin/python video/build.py render -j 7 --force   # 1080p30, chapters in parallel
.venv/bin/python video/build.py mux --force  # attach narration
.venv/bin/python video/build.py concat       # final/full.mp4
```

| File | Role |
|---|---|
| `narration/*.txt` + `narration-audio/*.mp3` | scripts and recordings (verified ≥ 0.94 word match) |
| `narration-audio/*.align.json` | measured start time of every paragraph (`align.py`) |
| `video/segments.py` | the beat plan above, as data |
| `video/diagrams.py` | flow-diagram layouts (frame fan-out, RTSP, HA, auth …) |
| `video/beats.py` | renderers: charts, partition map, wiring diagrams, HA card, CTA |
| `video/theme.py` | colours sampled from the web UI tokens |

- Beats change when the narrator actually starts a paragraph (whisper word
  timestamps), and each beat ends on an absolute timestamp, so there is no
  drift.
- Each chapter opens with a 2 s chapter card (the intro with a 3 s logo
  sting). The narration is delayed to match, and the end chapter holds for
  10 s for YouTube's end-screen elements.
- Outputs: `final/01-intro.mp4` … `final/07-end.mp4` and `final/full.mp4`
  (git-ignored).
- Wiring diagrams: ESP32-CAM on the ESP32-CAM-MB (Ch 1), the internal pin
  map of camera, SD (1-bit) and flash LED (Ch 1), and a USB-serial adapter
  wired to 5V/GND/U0R/U0T with the IO0→GND flashing jumper (Ch 5). Pins come
  from `firmware/include/board_profile.h`.
