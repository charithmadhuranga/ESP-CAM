# Phase 7 — MQTT and Home Assistant

**Goal:** a separate MQTT worker publishing availability, state and events on
a stable topic schema, with full Home Assistant discovery — never blocking
camera or storage.

**Spec:** §4.10, §4.11, §6.9, §9, §12 Phase 7 (+ amendment).
**Skills:** `visioncam-dev`, `visioncam-api-contract`.

---

## Entry criteria

Phase 4 exit gate green (events exist). Phase 6 recommended but not required.

**Read first:** SPEC §4.10/§4.11 and R1 `mqtt.cpp` via
`visioncam-reference-first`.

## Deliverables

1. `firmware/src/mqtt/` worker using ESP-IDF **`esp-mqtt`** (MQTT 3.1.1).
2. Bounded publish queue with documented overflow/coalescing.
3. Availability (retained `online` + retained Last Will `offline` ≥ QoS 1).
4. Full topic schema on `visioncam/{device_id}/…`.
5. Home Assistant discovery with stable IDs, birth-message handling,
   republish action and entity cleanup.
6. HA command handlers with payload validation.
7. Integrations UI (MQTT + Home Assistant cards).

## Ordered tasks

### 7.1 Worker

8. MQTT runs in **its own task**; camera/storage tasks never wait on the
   broker (AGENTS rule 11).
9. Check broker reachability before connecting (R1 pattern), then connect
   with exponential backoff + jitter.
10. Non-blocking bounded publish queue; QoS 0 for high-frequency telemetry,
    QoS 1 for important events; drop/coalesce low-priority telemetry when
    full; preserve event records locally regardless.
11. Retained `online` on connect; retained Last Will `offline`.
12. Subscribe to the command topic and to `homeassistant/status`.
13. Never publish secrets or binary images (deliberate divergence from R1).
    Snapshot paths are device-local only.

### 7.2 Topics (default base `visioncam/{device_id}`)

```
…/availability   …/state   …/status
…/motion/detected   …/motion/event
…/detection/detected   …/detection/event
…/recording/status   …/recording/event
…/snapshot/created
…/storage/used   …/storage/free   …/storage/percentage
…/wifi/rssi   …/ai/status
```

`device_id` is stable across reboots and firmware updates (generated once,
stored in NVS).

### 7.3 Home Assistant discovery

14. Default prefix `homeassistant`; retained discovery configs; stable
    `unique_id`s such as `visioncam_vcam_a1b2c3_motion`.
15. Entities: Camera online, Motion detected, Object detected, Person
    detected (only if the class exists), Recording active, AI enabled, SD
    mounted, Storage used/free/percentage, Wi-Fi RSSI, Inference time,
    Start/Stop recording buttons, Take snapshot button, Flash LED switch
    (only if the hardware mode is safe).
16. Device block: `identifiers`, `name`, `manufacturer`, `model`,
    `sw_version`.
17. Handle the HA birth message → republish discovery when HA reports
    `online`. Do **not** republish on every loop.
18. Remove obsolete entities by publishing an **empty retained** config when
    a feature is intentionally removed.
19. Provide a "Republish discovery" UI action; make discovery optional.
20. Command handlers validate payloads; any HTTP path involved still obeys
    device authentication.

### 7.4 API and UI

21. Endpoints §6.9: MQTT get/put/test/reconnect/test-publish, HA discovery
    republish, HA status.
22. **Integrations** page per `Integrations.png`: MQTT card (host/port,
    credentials, TLS, base topic, client ID, discovery prefix, connection
    test, save + reconnect, publish test, connection state), Home Assistant
    card (discovery enable, entity preview, republish, discovery status,
    birth status, broker setup instructions), Network Storage card
    (disabled until Phase 8).
23. Header `MQTT: Connected` pill — **only** on a real connection.

## Memory / timing / hardware notes

- TLS for MQTT needs substantial free heap — gate it like external AI.
- Keep payloads small; no image data.

## Tests and acceptance

- [ ] Broker connection succeeds; bad credentials produce a useful error.
- [ ] Reconnect works after a broker restart.
- [ ] Last Will publishes retained `offline`; retained `online` after
      reconnect.
- [ ] Motion and detection events publish on the documented topics.
- [ ] Discovery entities appear in Home Assistant.
- [ ] Entity unique IDs stable across reboot.
- [ ] HA birth message triggers discovery republish.
- [ ] Removed entities are cleaned up with an empty retained config.
- [ ] Broker outage does not block camera tasks or interrupt recording
      (measured).
- [ ] No secret or binary image ever published.

## Exit gate

Entities appear in Home Assistant, events publish, broker outage does not
interrupt recording.

## Docs updated

`docs/MQTT_HOME_ASSISTANT.md` (topics, entity table, setup guide),
`docs/API.md`, `docs/UI_UX_SPEC.md`, `CHANGELOG.md`.

## Risks / known gaps

- Discovery retained messages must not be re-sent every loop or HA will
  flap — verify with a broker trace.
- QoS 1 + retain choices must be recorded; R1's divergences are deliberate.
