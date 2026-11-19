import React from "react";
import { Card } from "../components/Card";
import { Button } from "../components/Button";
import { useToast } from "../hooks/useToast";
import { ConfirmDialog } from "../components/ConfirmDialog";
import {
  startRecording,
  stopRecording,
  getRecordings,
  deleteRecording,
} from "../lib/api";
import type { RecordingInfo } from "../lib/types";

type Filter = "all" | "continuous" | "manual" | "event";

const FILTERS: { id: Filter; label: string }[] = [
  { id: "all", label: "All" },
  { id: "continuous", label: "Continuous" },
  { id: "manual", label: "Manual" },
  { id: "event", label: "Event" },
];

type PageState = "loading" | "empty" | "error" | "success";

export function Recordings() {
  const { show } = useToast();
  const [items, setItems] = React.useState<RecordingInfo[]>([]);
  const [hasMore, setHasMore] = React.useState(false);
  const [offset, setOffset] = React.useState(0);
  const [limit] = React.useState(12);
  const [filter, setFilter] = React.useState<Filter>("all");
  const [pageState, setPageState] = React.useState<PageState>("loading");
  const [error, setError] = React.useState<string | null>(null);
  const [confirmOpen, setConfirmOpen] = React.useState(false);
  const [pendingId, setPendingId] = React.useState<string | null>(null);
  const [isRecording] = React.useState(false);
  const [successMsg, setSuccessMsg] = React.useState<string | null>(null);

  const load = async () => {
    setPageState("loading");
    setError(null);
    try {
      await loadList();
    } catch (err: any) {
      setPageState("error");
      setError(err?.message ?? "Failed to load recordings");
    }
  };

  const loadList = async () => {
    const data = await getRecordings({
      offset,
      limit,
    });
    const filtered = filter === "all" ? data.items : data.items.filter((i) => i.category === filter);
    setItems(filtered);
    setHasMore(data.has_more);
    setPageState(filtered.length === 0 ? "empty" : "success");
  };

  React.useEffect(() => {
    load();
  }, [offset, filter, show]);

  const handleLoadMore = () => {
    if (!hasMore) return;
    setOffset((o) => o + limit);
  };

  const handleStart = async () => {
    try {
      const result = await startRecording();
      show("Recording started", "success");
      setSuccessMsg(`Recording ${result.recording_id} started`);
      setTimeout(() => load(), 800);
    } catch (err: any) {
      show(err?.message ?? "Start failed", "error");
    }
  };

  const handleStop = async () => {
    try {
      await stopRecording();
      show("Recording stopped", "success");
      setSuccessMsg("Recording stopped");
      setTimeout(() => load(), 800);
    } catch (err: any) {
      show(err?.message ?? "Stop failed", "error");
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
      await deleteRecording(pendingId);
      show("Deleted", "success");
      setPendingId(null);
      setTimeout(() => load(), 400);
    } catch (err: any) {
      setError(err?.message ?? "Delete failed");
    } finally {
      setPendingId(null);
    }
  };

  const handleDownload = (recordingId: string) => {
    window.location.href = `/recordings?action=download&id=${recordingId}`;
  };

  const handlePlay = (recordingId: string) => {
    window.location.href = `/recordings?action=play&id=${recordingId}`;
  };

  if (pageState === "loading") {
    return (
      <div className="page recordings-page">
        <h2 className="page-title">Recordings</h2>
        <div className="skeleton card-skeleton" style={{ height: 560 }} />
      </div>
    );
  }
  if (pageState === "error") {
    return (
      <div className="page recordings-page">
        <h2 className="page-title">Recordings</h2>
        <div className="error-text">{error}</div>
        <Button variant="primary" onClick={load}>Retry</Button>
      </div>
    );
  }

  return (
    <div className="page recordings-page">
      <div className="page-header">
        <h2 className="page-title">Recordings</h2>
        <div className="page-actions">
          <Button
            variant={isRecording ? "danger" : "primary"}
            onClick={isRecording ? handleStop : handleStart}
          >
            {isRecording ? "Stop Recording" : "Start Recording"}
          </Button>
        </div>
      </div>

      {successMsg && (
        <div className="toast toast--success" role="status">
          {successMsg}
        </div>
      )}

      <div className="card">
        <h3 className="card-title">Recording Status</h3>
        <div className="status-grid">
          <div className="status-item">
            <span className="status-label">State</span>
            <span
              className={`status-value ${isRecording ? "recording" : "idle"}`}
            >
              {isRecording ? "Recording" : "Idle"}
            </span>
          </div>
        </div>
      </div>

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

      <div className="gallery">
        {items.map((item) => (
          <Card key={item.recording_id} className="gallery-card">
            <div className="gallery-meta">
              <div className="gallery-id">{item.recording_id}</div>
              <div className="gallery-ts">
                {new Date(item.start_time * 1000).toLocaleString()}
              </div>
              <div className="gallery-duration">
                {item.duration_s}s • {item.frame_count} frames
              </div>
              <div className="gallery-size">
                {(item.size_bytes / 1024).toFixed(1)} KB
              </div>
              <div className="gallery-status">
                {item.recovered ? "Recovered" : item.status}
              </div>
            </div>
            <div className="gallery-actions">
              <Button variant="secondary" onClick={() => handlePlay(item.recording_id)}>
                Play
              </Button>
              <Button variant="secondary" onClick={() => handleDownload(item.recording_id)}>
                Download
              </Button>
              <Button variant="danger" onClick={() => openDelete(item.recording_id)}>
                Delete
              </Button>
            </div>
          </Card>
        ))}
        {items.length === 0 && (
          <div className="empty-text">No recordings yet.</div>
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
        title="Delete recording"
        message="This cannot be undone."
        confirmLabel="Delete"
        confirmType="danger"
        typedConfirm="DELETE"
      />
    </div>
  );
}
