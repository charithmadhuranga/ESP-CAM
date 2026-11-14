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

6. `HttpUploadStorageBackend`: `begin / isAvailable / writeFile /
   removeFile` per the §4.9 interface, plus an enqueue-completed-file path.
7. Trigger on segment/snapshot close (R1 pattern) — **never** stream frames
   synchronously to a remote server.
8. Config: enable, backend type, server URL, credentials/token (masked in
   the UI, never returned by GET — return `password_configured: true`),
   destination path, upload policy, retry count, retry interval, TLS
   verification, upload queue limit.
9. **Bounded** queue with documented overflow behaviour: drop oldest
   non-critical or stop enqueueing and report — never grow without limit.
10. Retry with exponential backoff; failures raise
    `NetworkStorageUploadFailed` and are listed in the UI.
11. Heap gate: check free heap and largest free block before starting a TLS
    session (R1: ≈64 KB threshold); back off otherwise.
12. Local SD stays authoritative unless the user explicitly changes policy.
    Delete-after-upload is opt-in, default off.

### 8.2 API and UI

13. Endpoints §6.9: network-storage get/put/test, queue status.
14. **Integrations › Network Storage** card: backend, URL, auth (masked),
    test connection, upload policy, queue status, retry/failure list.

## Memory / timing / hardware notes

- The upload worker runs at low priority and must never compete with the
  storage writer or stream.
- Queue entries reference media IDs/paths on SD; do not copy file bodies
  into the queue.

## Tests and acceptance

- [ ] Files upload after segment closure.
- [ ] Network failure preserves local files (nothing deleted locally).
- [ ] Queue remains bounded under sustained failure — verify by filling it.
- [ ] TLS session refused cleanly when free heap is below the gate.
- [ ] Test connection reports real reachability, not a saved config.
- [ ] Secrets masked in the UI and absent from GET responses and logs.

## Exit gate

Completed files upload after segment close; network failure preserves local
files; queue bounded.

## Docs updated

`docs/STORAGE_AND_RECORDING.md`, `docs/API.md`, `docs/UI_UX_SPEC.md`,
`CHANGELOG.md`.

## Risks / known gaps

- FTP/WebDAV deferred (Phase 8 amendment) — do not implement them here.
- NAS integration is preferred to be an external gateway, not an on-device
  filesystem client.
