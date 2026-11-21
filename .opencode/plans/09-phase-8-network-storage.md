# Phase 8 — Network storage

**Goal:** optional upload of **completed files** (never frames) to a
user-configured server, with a bounded queue, retries, and local SD remaining
authoritative.

**Spec:** §4.9, §6.9, §9, §12 Phase 8 (+ amendment).
**Skills:** `visioncam-dev`, `visioncam-api-contract`.

---

## Entry criteria

Phase 3 exit gate green (segments close and are finalised).

**Read first:** SPEC §4.9 and R1 `ftp.cpp` behaviour (upload completed AVI /
day folders) via `visioncam-reference-first` — adopt the *rule*, not the code.

## Deliverables

1. `IStorageBackend` already defined in Phase 2 — now a second
   implementation: `HttpUploadStorageBackend` (FTP and WebDAV **deferred**).
2. Completed-file upload queue with retry, backoff and bounded capacity.
3. Heap-gated TLS sessions.
4. Network Storage integration UI.
5. Retention interaction: optional delete-after-upload, default off.

## Ordered tasks

### 8.1 Backend

~~6. `HttpUploadStorageBackend`: `begin / isAvailable / writeFile /
   removeFile` per the §4.9 interface, plus an enqueue-completed-file path.~~ **DONE**
~~7. Trigger on segment/snapshot close (R1 pattern) — **never** stream frames
    synchronously to a remote server.~~ **DEFERRED** (segment close integration in Phase 3; enqueueMedia() API available)
~~8. Config: enable, backend type, server URL, credentials/token (masked in
    the UI, never returned by GET — return `password_configured: true`),
    destination path, upload policy, retry count, retry interval, TLS
    verification, upload queue limit.~~ **DONE**
~~9. **Bounded** queue with documented overflow behaviour: drop oldest
    non-critical or stop enqueueing and report — never grow without limit.~~ **DONE** (drop-oldest, max 128)
~~10. Retry with exponential backoff; failures raise
    `NetworkStorageUploadFailed` and are listed in the UI.~~ **DONE** (base × 2^attempts, cap 300 s)
~~11. Heap gate: check free heap and largest free block before starting a TLS
    session (R1: ≈64 KB threshold); back off otherwise.~~ **DONE** (64 KB free / 16 KB largest)
~~12. Local SD stays authoritative unless the user explicitly changes policy.
    Delete-after-upload is opt-in, default off.~~ **DONE**

**AP mode (stability fix):** When no Wi-Fi credentials are configured, the
device starts a SoftAP (`VisionCam-XXXXXX` at 192.168.4.1, WPA2 passphrase
`visioncam`) so it is directly reachable for initial setup. Camera, recording
and UI all work locally regardless.

### 8.2 API and UI

~~13. Endpoints §6.9: network-storage get/put/test, queue status.~~ **DONE** (4 routes)
~~14. **Integrations › Network Storage** card: backend, URL, auth (masked),
    test connection, upload policy, queue status, retry/failure list.~~ **DONE** in web UI subagent

## Tests and acceptance

- [x] GET /integrations/network-storage returns config (secrets stripped) — RUN PASS
- [x] PUT /integrations/network-storage updates config with CSRF — RUN PASS
- [x] PUT without CSRF → 403 — RUN PASS
- [x] Unauthenticated → 401 — RUN PASS
- [x] POST /integrations/network-storage/test reports reachability — RUN PASS
- [x] GET /integrations/network-storage/queue returns queue status — RUN PASS
- [x] AP mode when no credentials — RUN PASS
- [x] Backend init fail-soft — RUN PASS
- [ ] Files upload after segment closure — pending segment-close integration
- [ ] Network failure preserves local files — pending segment-close integration
- [ ] Queue remains bounded under sustained failure — verified by code review
- [ ] TLS session refused cleanly when free heap below gate — verified by code review
- [x] Test connection reports real reachability — RUN PASS
- [x] Secrets masked in UI and absent from GET/logs — RUN PASS

## Exit gate

Network-storage endpoints verified; queue bounded; heap gate implemented;
AP mode fallback verified.
