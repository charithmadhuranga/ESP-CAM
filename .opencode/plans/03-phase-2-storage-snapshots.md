# Phase 2 — Storage and snapshots

**Goal:** a single storage manager owning all filesystem access, a rebuildable
JSONL media index, and a paginated snapshot gallery with viewer, download and
delete.

**Spec:** §4.5, §4.8 (directory layout, index design, retention), §6.5, §6.7,
§7, §10 (9, 10), §12 Phase 2.
**Skills:** `visioncam-dev`, `visioncam-api-contract`,
`visioncam-hardware-validation`.

---

## Entry criteria

Phase 1 exit gate green; SD card present and mounting (Phase 0).

## Deliverables

1. `firmware/src/storage/` StorageManager — **the only module that performs
   filesystem operations** (AGENTS rule 10).
2. `SdStorageBackend` implementing `IStorageBackend` (§4.9).
3. Full SD directory layout per §4.8.
4. Append-only JSONL media index + event index, versioned, rebuildable.
5. Snapshot manager (sources, timestamped names, event association,
   thumbnails, dedupe within cooldown).
6. Gallery API + UI: filters, date range, pagination, full-screen viewer,
   metadata panel, download, delete with confirmation.
7. Storage status, index rebuild, retention policy with preview.

## Ordered tasks

### 2.1 Storage manager

1. `IStorageBackend` + `SdStorageBackend`: mount/unmount state, capacity
   (total/used/free), directory listing, stat, read, write, rename, unlink,
   mkdir. POSIX via the FATFS VFS.
2. Enforce the §4.8 layout:
   `/system/{device.json,config-backup.json,index/*.jsonl}`,
   `/snapshots/YYYY/MM/DD/{manual,motion,detection,timelapse}/`,
   `/recording/{continuous,events,manual}/YYYY/MM/DD/`,
   `/timelapse/{project_id}/{frames,output,project.json}`,
   `/thumbnails/`, `/models/{staging,active,backup}/`, `/logs/`.
3. Opaque ID layer: every public API takes `med-…`/`rec-…`/`evt-…` IDs, never
   a path (AGENTS rule 15). Reject traversal and symlink-like manipulation
   even though FAT has no symlinks.
4. Write media to a temporary/in-progress name; finalise + rename after
   close. `fsync` where durability matters. Never delete open files.
5. Sector-aligned buffering: stage writes in multiples of the SD sector size
   (8 KB) and confirm with `setvbuf`/large `fwrite` that whole blocks reach
   the card — measure (§1A.10 rule 3).
6. Bounded vectors of directory/file entries in **PSRAM**; paginate; never a
   full listing in internal RAM.

### 2.2 Indexes

7. JSONL media index and event index per §4.8, `schema_version` on every
   record. Append-only; validate each line independently; ignore a malformed
   trailing record on recovery; compact via atomic temp-file replacement.
8. Treat the filesystem as source of truth; indexes are rebuildable caches.
9. `POST /storage/rebuild-index` scans directories and rewrites the index.
10. Never load the whole media library into RAM — paginate API results.

### 2.3 Snapshots

11. `firmware/src/snapshot/`: capture on manual request, with configurable
    resolution and JPEG quality; timestamped filename under the source
    folder; optional thumbnail via R2's conversion helpers (do not write a
    converter); media record appended to the index.
12. Suppress duplicate snapshots for repeated detections inside the cooldown.
13. Endpoints §6.5: list (paginated + filters + date range), metadata,
    image bytes, download, delete. Image route authenticated.

### 2.4 Retention (initial)

14. Retention policy config (days per category, max used percent) with
    read/write endpoints. Deletion logic runs but is exercised in Phase 9 —
    here it must at least refuse to delete active/open segments and the only
    copy of an active model, log every automatic deletion, and provide a
    preview of candidates.

### 2.5 UI

15. **Snapshots** page: filter pills (All, Manual, Motion, Detection,
    Timelapse), date range, 5-column grid → responsive, pagination, full-
    screen viewer with previous/next, metadata panel, event link, download,
    delete with confirmation.
16. **Storage** page: capacity bar, category breakdown, SD health, breadcrumb
    file list with download/delete, retention policy form, Rebuild index,
    Format card (typed confirmation — never auto-format).

## Memory / timing / hardware notes

- SD write contention with the future recorder is the reason the storage
  writer becomes its own task in Phase 3 — keep the API write path
  non-blocking here.
- Do not run cleanup while the filesystem is busy or in an error state.
- Mount failure: clear error, preserve config, disable storage-dependent
  features, keep diagnostic access (SPEC §2.3).

## Tests and acceptance

- [ ] SD mount/unmount state correct in status and UI.
- [ ] Snapshot writes, survives reboot, downloads as a valid JPEG.
- [ ] Directory and API pagination work; no full listing in RAM.
- [ ] Delete works and updates the index.
- [ ] Index rebuild recovers missing entries.
- [ ] Corrupt trailing JSONL entry is ignored on load.
- [ ] Full-card condition handled safely (retention runs, else recording
      stops with a critical warning — never silent corruption).
- [ ] Active recording is never deleted.
- [ ] Path traversal attempts rejected on every media route.
- [ ] Unauthenticated image/download requests rejected.

## Exit gate

Snapshots survive reboot, gallery paginates, index rebuilds, retention
preview correct.

## Docs updated

`docs/STORAGE_AND_RECORDING.md`, `docs/API.md`, `docs/UI_UX_SPEC.md`,
`CHANGELOG.md`.

## Risks / known gaps

- C stdio buffering on FAT may split sector-aligned writes — verify on the
  card, not by reading the code.
- Thumbnail generation cost at VGA/SVGA must be measured before enabling by
  default.
