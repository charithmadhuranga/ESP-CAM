import React from "react";
import { Card } from "../components/Card";
import { StatCard } from "../components/StatCard";
import { Row } from "../components/Row";
import { StatusPill } from "../components/StatusPill";
import { Button } from "../components/Button";
import { ProgressBar } from "../components/ProgressBar";
import { VideoPlayer } from "../components/VideoPlayer";
import { useToast } from "../hooks/useToast";
import { getStatus, getCameraStatus } from "../lib/api";

export function Overview() {
  const { show } = useToast();
  const [status, setStatus] = React.useState<{
    wifi: { state: string; configured: boolean; radio_up: boolean; rssi: number };
    camera: { initialised: boolean; framesize: number };
    time_synced: boolean;
    heap_free_bytes: number;
    largest_block_bytes: number;
  } | null>(null);
  const [camera, setCamera] = React.useState<{
    initialised: boolean;
    sensor: string;
    framesize: number;
    quality: number;
    jpeg_capacity_bytes: number;
  } | null>(null);
  const [loading, setLoading] = React.useState(true);
  const [error, setError] = React.useState<string | null>(null);

  React.useEffect(() => {
    let cancelled = false;
    Promise.all([getStatus(), getCameraStatus()])
      .then(([s, c]) => {
        if (cancelled) return;
        setStatus(s);
        setCamera(c);
        setLoading(false);
      })
      .catch((err) => {
        if (cancelled) return;
        setError(err.message);
        setLoading(false);
      });
    return () => { cancelled = true; };
  }, [show]);

  if (loading) return <SkeletonPage />;
  if (error) return <ErrorPage error={error} onRetry={() => window.location.reload()} />;
  if (!status || !camera) return <EmptyPage />;

  return (
    <div className="page">
      <h2 className="page-title">Overview</h2>
      <div className="page-grid">
        {/* Stat cards */}
        <div className="stat-grid">
          <StatCard label="Camera" variant={camera.initialised ? "success" : "danger"} value={camera.initialised ? "Online" : "Offline"} />
          <StatCard label="Recording" variant="default" value="Idle" />
          <StatCard label="Motion today" variant="default" value="0" />
          <StatCard label="AI events today" variant="default" value="0" />
        </div>

        {/* Live preview + device health */}
        <div className="two-col">
          <Card title="Live preview" className="live-card">
            <div className="live-wrapper">
              <VideoPlayer
                streamUrl="/camera/stream"
                recording={false}
                width={480}
                height={360}
              />
            </div>
            <div className="live-actions">
              <Button variant="primary" onClick={() => show("Capture not wired yet", "info")}>
                Capture
              </Button>
              <Button variant="secondary" onClick={() => show("Record not wired yet", "info")}>
                Record
              </Button>
            </div>
          </Card>

          <Card title="Device health">
            <Row label="Wi-Fi">
              <StatusPill variant={status.wifi.state === "Connected" ? "success" : "default"}>
                {status.wifi.state}
              </StatusPill>
            </Row>
            <Row label="RSSI" value={`${status.wifi.rssi} dBm`} />
            <Row label="NTP synced" value={status.time_synced ? "Yes" : "No"} />
            <Row label="Free heap" value={`${Math.round(status.heap_free_bytes / 1024)} KB`} />
            <Row label="Largest block" value={`${Math.round(status.largest_block_bytes / 1024)} KB`} />
            <ProgressBar
              value={Math.round((status.largest_block_bytes / status.heap_free_bytes) * 100)}
              label="Heap fragmentation"
            />
          </Card>
        </div>

        {/* Recent events / snapshots (2-column row) */}
        <div className="two-col">
          <Card title="Recent events">
            <div className="empty-state">
              <span className="empty-text">No events yet</span>
            </div>
          </Card>
          <Card title="Recent snapshots">
            <div className="empty-state">
              <span className="empty-text">No snapshots yet</span>
            </div>
          </Card>
        </div>
      </div>
    </div>
  );
}

function SkeletonPage() {
  return (
    <div className="page">
      <h2 className="page-title">Overview</h2>
      <div className="skeleton-grid">
        <div className="skeleton stat-skeleton"></div>
        <div className="skeleton stat-skeleton"></div>
        <div className="skeleton stat-skeleton"></div>
        <div className="skeleton stat-skeleton"></div>
        <div className="skeleton card-skeleton"></div>
        <div className="skeleton card-skeleton"></div>
      </div>
    </div>
  );
}

function ErrorPage({ error, onRetry }: { error: string; onRetry: () => void }) {
  return (
    <div className="page error-state">
      <div className="empty-state">
        <span className="empty-text">Error loading overview</span>
        <span className="empty-sub">{error}</span>
        <Button variant="secondary" onClick={onRetry}>Retry</Button>
      </div>
    </div>
  );
}

function EmptyPage() {
  return (
    <div className="page empty-state">
      <div className="empty-state">
        <span className="empty-text">No data available</span>
      </div>
    </div>
  );
}
