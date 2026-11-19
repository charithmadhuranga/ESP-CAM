import React from "react";
import { Card } from "../components/Card";
import { Button } from "../components/Button";
import { Field } from "../components/Field";
import { useToast } from "../hooks/useToast";
import { ConfirmDialog } from "../components/ConfirmDialog";
import {
  captureSnapshot,
  deleteMedia,
  getMediaList,
} from "../lib/api";
import type { MediaRecord } from "../lib/types";

type Filter = "all" | "manual" | "motion" | "detection" | "timelapse";

const FILTERS: { id: Filter; label: string }[] = [
  { id: "all", label: "All" },
  { id: "manual", label: "Manual" },
  { id: "motion", label: "Motion" },
  { id: "detection", label: "Detection" },
  { id: "timelapse", label: "Timelapse" },
];

type PageState = "loading" | "empty" | "error" | "success";

export function Snapshots() {
  const { show } = useToast();
  const [items, setItems] = React.useState<MediaRecord[]>([]);
  const [hasMore, setHasMore] = React.useState(false);
  const [offset, setOffset] = React.useState(0);
  const [limit] = React.useState(12);
  const [filter, setFilter] = React.useState<Filter>("all");
  const [from, setFrom] = React.useState("");
  const [to, setTo] = React.useState("");
  const [pageState, setPageState] = React.useState<PageState>("loading");
  const [error, setError] = React.useState<string | null>(null);
  const [confirmOpen, setConfirmOpen] = React.useState(false);
  const [pendingId, setPendingId] = React.useState<string | null>(null);
  const [successMsg, setSuccessMsg] = React.useState<string | null>(null);

  const load = async () => {
    setPageState("loading");
    setError(null);
    try {
      const data = await getMediaList({
        offset,
        limit,
        category: filter === "all" ? undefined : filter,
      });
      setItems(data.items);
      setHasMore(data.has_more);
      setPageState(data.items.length === 0 ? "empty" : "success");
    } catch (err: any) {
      setPageState("error");
      setError(err?.message ?? "Failed to load snapshots");
    }
  };

  React.useEffect(() => {
    load();
  }, [offset, filter, show]);

  const handleLoadMore = () => {
    if (!hasMore) return;
    setOffset((o) => o + limit);
  };

  const handleCapture = async () => {
    setPageState("loading");
    try {
      const result = await captureSnapshot();
      if (result.suppressed) {
        show(`Capture suppressed: ${result.reason}`, "info");
        setPageState("success");
        return;
      }
      setSuccessMsg(`Snapshot captured: ${result.media_id}`);
      show("Snapshot captured", "success");
      setTimeout(() => load(), 1200);
    } catch (err: any) {
      setPageState("error");
      setError(err?.message ?? "Capture failed");
    }
  };

  const openDelete = (id: string) => {
    setPendingId(id);
    setConfirmOpen(true);
  };

  const handleDelete = async () => {
    if (!pendingId) return;
    setConfirmOpen(false);
    try {
      await deleteMedia(pendingId);
      show("Deleted", "success");
      setPendingId(null);
      setTimeout(() => load(), 400);
    } catch (err: any) {
      setError(err?.message ?? "Delete failed");
    } finally {
      setPendingId(null);
    }
  };

  const handleDownload = (mediaId: string) => {
    window.location.href = `/snapshot?id=${mediaId}`;
  };

  if (pageState === "loading") {
    return (
      <div className="page snapshots-page">
        <h2 className="page-title">Snapshots</h2>
        <div className="skeleton card-skeleton" style={{ height: 560 }} />
      </div>
    );
  }
  if (pageState === "error") {
    return (
      <div className="page snapshots-page">
        <h2 className="page-title">Snapshots</h2>
        <div className="error-text">{error}</div>
        <Button variant="primary" onClick={load}>Retry</Button>
      </div>
    );
  }

  return (
    <div className="page snapshots-page">
      <div className="page-header">
        <h2 className="page-title">Snapshots</h2>
        <div className="page-actions">
          <Button variant="primary" onClick={handleCapture}>
            New Snapshot
          </Button>
        </div>
      </div>

      {successMsg && (
        <div className="toast toast--success" role="status">
          {successMsg}
        </div>
      )}

      <div className="filters">
        {FILTERS.map((f) => (
          <Button
            key={f.id}
            variant="secondary"
            className={filter === f.id ? "btn--active" : ""}
            onClick={() => setFilter(f.id)}
          >
            {f.label}
          </Button>
        ))}
      </div>

      <div className="date-range">
        <Field label="From" id="snap-from" type="date" value={from} onChange={(e) => setFrom(e.target.value)} />
        <Field label="To" id="snap-to" type="date" value={to} onChange={(e) => setTo(e.target.value)} />
      </div>

      <div className="gallery">
        {items.map((item) => (
          <Card key={item.media_id} className="gallery-card">
            <div className="gallery-thumb-wrap">
              <img
                src={`/snapshot?id=${item.media_id}`}
                alt={`Snapshot ${item.media_id}`}
                className="gallery-thumb"
                onError={(e) => {
                  e.currentTarget.src = "";
                }}
              />
              <div className="gallery-badge">{item.category}</div>
            </div>
            <div className="gallery-meta">
              <div className="gallery-id">{item.media_id}</div>
              <div className="gallery-ts">{item.created_at}</div>
              <div className="gallery-size">
                {(item.size / 1024).toFixed(1)} KB
              </div>
            </div>
            <div className="gallery-actions">
              <Button
                variant="secondary"
                onClick={() => handleDownload(item.media_id)}
              >
                Download
              </Button>
              <Button
                variant="danger"
                onClick={() => openDelete(item.media_id)}
              >
                Delete
              </Button>
            </div>
          </Card>
        ))}
        {items.length === 0 && (
          <div className="empty-text">No snapshots yet.</div>
        )}
      </div>

      {hasMore && (
        <div className="pager">
          <Button variant="secondary" onClick={handleLoadMore}>
            Load more
          </Button>
        </div>
      )}

      <ConfirmDialog
        open={confirmOpen}
        onClose={() => setConfirmOpen(false)}
        onConfirm={handleDelete}
        title="Delete snapshot"
        message="This cannot be undone."
        confirmLabel="Delete"
        confirmType="danger"
        typedConfirm="DELETE"
      />
    </div>
  );
}
