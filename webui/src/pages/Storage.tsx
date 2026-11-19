import React from "react";
import { Card } from "../components/Card";
import { Button } from "../components/Button";
import { Field } from "../components/Field";
import { useToast } from "../hooks/useToast";
import { ConfirmDialog } from "../components/ConfirmDialog";
import {
  getRetention,
  getStorageStatus,
  previewRetention,
  putRetention,
  rebuildIndex,
  runRetention,
} from "../lib/api";

type PageState = "loading" | "error" | "success";

export function Storage() {
  const { show } = useToast();
  const [status, setStatus] = React.useState<{
    mounted: boolean;
    total_bytes: number;
    used_bytes: number;
    free_bytes: number;
    media_count: number;
  } | null>(null);
  const [retention, setRetention] = React.useState<any>(null);
  const [preview, setPreview] = React.useState<{
    count: number;
    candidates: any[];
  } | null>(null);
  const [pageState, setPageState] = React.useState<PageState>("loading");
  const [error, setError] = React.useState<string | null>(null);
  const [confirmOpen, setConfirmOpen] = React.useState(false);
  const [runConfirmOpen, setRunConfirmOpen] = React.useState(false);
  const [successMsg, setSuccessMsg] = React.useState<string | null>(null);

  const load = async () => {
    setPageState("loading");
    try {
      const [st, rt, pv] = await Promise.all([
        getStorageStatus(),
        getRetention(),
        previewRetention(),
      ]);
      setStatus(st);
      setRetention(rt);
      setPreview(pv);
      setPageState("success");
    } catch (err: any) {
      setPageState("error");
      setError(err?.message ?? "Failed to load storage info");
    }
  };

  React.useEffect(() => {
    load();
  }, [show]);

  const handleRebuild = async () => {
    try {
      await rebuildIndex();
      show("Index rebuilt", "success");
      setSuccessMsg("Index rebuilt");
      setTimeout(() => load(), 800);
    } catch (err: any) {
      setError(err?.message ?? "Rebuild failed");
    }
  };

  const handleSaveRetention = async (e: React.FormEvent) => {
    e.preventDefault();
    try {
      const body: any = {};
      if (retention) {
        body.retention_days_manual = retention.retention_days_manual;
        body.retention_days_motion = retention.retention_days_motion;
        body.retention_days_detection = retention.retention_days_detection;
        body.retention_days_timelapse = retention.retention_days_timelapse;
        body.max_used_percent = retention.max_used_percent;
        body.enabled = retention.enabled;
      }
      const updated = await putRetention(body);
      setRetention(updated);
      show("Retention saved", "success");
      setSuccessMsg("Retention saved");
    } catch (err: any) {
      setError(err?.message ?? "Save failed");
    }
  };

  const handleRunRetention = async () => {
    setRunConfirmOpen(false);
    try {
      const result = await runRetention();
      show(`Retention deleted ${result.deleted} file(s)`, "success");
      setSuccessMsg(`Deleted ${result.deleted} file(s)`);
      setTimeout(() => load(), 800);
    } catch (err: any) {
      setError(err?.message ?? "Retention run failed");
    }
  };

  const totalGb = status
    ? (status.total_bytes / (1024 * 1024 * 1024)).toFixed(2)
    : "—";
  const usedMb = status
    ? (status.used_bytes / (1024 * 1024)).toFixed(1)
    : "—";
  const freeMb = status
    ? (status.free_bytes / (1024 * 1024)).toFixed(1)
    : "—";
  const pct = status
    ? (status.used_bytes / Math.max(1, status.total_bytes)) * 100
    : 0;

  if (pageState === "loading") {
    return (
      <div className="page storage-page">
        <h2 className="page-title">Storage</h2>
        <div className="skeleton card-skeleton" style={{ height: 560 }} />
      </div>
    );
  }
  if (pageState === "error") {
    return (
      <div className="page storage-page">
        <h2 className="page-title">Storage</h2>
        <div className="error-text">{error}</div>
        <Button variant="primary" onClick={load}>Retry</Button>
      </div>
    );
  }

  return (
    <div className="page storage-page">
      <h2 className="page-title">Storage</h2>

      <div className="grid-2">
        <Card title="SD card">
          <div className="storage-meta">
            <Row label="Mounted">
              <span
                className={`status-value ${status?.mounted ? "ok" : "bad"}`}
              data-testid="storage-mounted"
              >{status?.mounted ? "Yes" : "No"}</span>
            </Row>
            <Row label="Capacity">
              <span data-testid="storage-total">{totalGb} GB</span>
            </Row>
            <Row label="Used">
              <span data-testid="storage-used">{usedMb} MB</span>
            </Row>
            <Row label="Free">
              <span data-testid="storage-free">{freeMb} MB</span>
            </Row>
            <Row label="Media files">
              <span data-testid="storage-media-count">{status?.media_count}</span>
            </Row>
          </div>
          <div className="storage-bar-wrap">
            <div className="storage-bar-label">Used {pct.toFixed(1)}%</div>
            <div className="storage-bar">
              <div
                className="storage-bar-fill"
                style={{ width: `${Math.min(pct, 100)}%` }}
              />
            </div>
          </div>
          <div className="storage-actions">
            <Button variant="secondary" onClick={handleRebuild}>
              Rebuild index
            </Button>
          </div>
        </Card>

        <Card title="Retention policy">
          <form onSubmit={handleSaveRetention} className="retention-form">
            <div className="form-row">
              <Field
                label="Manual (days)"
                id="ret-days-manual"
                type="number"
                min={0}
                value={retention?.retention_days_manual ?? 0}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, retention_days_manual: Number(e.target.value) || 0 });
                }}
              />
              <Field
                label="Motion (days)"
                id="ret-days-motion"
                type="number"
                min={0}
                value={retention?.retention_days_motion ?? 0}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, retention_days_motion: Number(e.target.value) || 0 });
                }}
              />
              <Field
                label="Detection (days)"
                id="ret-days-detection"
                type="number"
                min={0}
                value={retention?.retention_days_detection ?? 0}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, retention_days_detection: Number(e.target.value) || 0 });
                }}
              />
              <Field
                label="Timelapse (days)"
                id="ret-days-timelapse"
                type="number"
                min={0}
                value={retention?.retention_days_timelapse ?? 0}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, retention_days_timelapse: Number(e.target.value) || 0 });
                }}
              />
            </div>
            <div className="form-row">
              <Field
                label="Max card used (%)"
                id="ret-max-pct"
                type="number"
                min={0}
                max={100}
                value={retention?.max_used_percent ?? 0}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, max_used_percent: Number(e.target.value) || 0 });
                }}
              />
              <Field
                label="Enabled"
                id="ret-enabled"
                type="checkbox"
                checked={retention?.enabled ?? true}
                onChange={(e) => {
                  if (retention)
                    setRetention({ ...retention, enabled: e.target.checked });
                }}
              />
            </div>
            <Button variant="primary" type="submit">
              Save retention
            </Button>
          </form>
        </Card>
      </div>

      <Card title="Retention preview">
        <div className="preview-list">
          {preview?.candidates.map((c) => (
            <div key={c.media_id} className="preview-item">
              <span className="preview-id">{c.media_id}</span>
              <span className="preview-cat">{c.category}</span>
              <span className="preview-age">{c.age_days} days old</span>
              <span className="preview-size">{(c.size / 1024).toFixed(1)} KB</span>
            </div>
          ))}
          {preview?.count === 0 && (
            <div className="empty-text">No files eligible for deletion.</div>
          )}
        </div>
        <div className="preview-actions">
          <Button
            variant="danger"
            onClick={() => setRunConfirmOpen(true)}
          >
            Run retention
          </Button>
        </div>
      </Card>

      {successMsg && (
        <div className="toast toast--success" role="status">
          {successMsg}
        </div>
      )}

      <ConfirmDialog
        open={confirmOpen}
        onClose={() => setConfirmOpen(false)}
        onConfirm={handleRebuild}
        title="Rebuild index"
        message="Rescans the card and rewrites the media index. Existing media IDs are preserved."
        confirmLabel="Rebuild"
        confirmType="secondary"
      />

      <ConfirmDialog
        open={runConfirmOpen}
        onClose={() => setRunConfirmOpen(false)}
        onConfirm={handleRunRetention}
        title="Run retention"
        message="Deletes files older than the configured retention days. This cannot be undone."
        confirmLabel="Delete now"
        confirmType="danger"
      />
    </div>
  );
}

function Row({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="storage-row">
      <span className="storage-row-label">{label}</span>
      <span className="storage-row-value">{children}</span>
    </div>
  );
}
