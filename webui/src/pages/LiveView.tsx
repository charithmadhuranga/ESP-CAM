import React from "react";
import { Card } from "../components/Card";
import { Row } from "../components/Row";
import { StatusPill } from "../components/StatusPill";
import { Button } from "../components/Button";
import { Slider } from "../components/Slider";
import { ProgressBar } from "../components/ProgressBar";
import { VideoPlayer } from "../components/VideoPlayer";
import { useToast } from "../hooks/useToast";
import { getCameraStatus } from "../lib/api";

export function LiveView() {
  const { show } = useToast();
  const [camera, setCamera] = React.useState<{
    initialised: boolean;
    sensor: string;
    framesize: number;
    quality: number;
    jpeg_capacity_bytes: number;
  } | null>(null);
  const [fps, setFps] = React.useState(0);
  const [loading, setLoading] = React.useState(true);
  const [error, setError] = React.useState<string | null>(null);

  React.useEffect(() => {
    let cancelled = false;
    getCameraStatus()
      .then((c) => {
        if (cancelled) return;
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

  // Simulated FPS ticker (real value comes from /metrics in later phases)
  React.useEffect(() => {
    if (!camera?.initialised) return;
    const t = setInterval(() => setFps((f) => (f + 0.5) % 31), 1000);
    return () => clearInterval(t);
  }, [camera]);

  if (loading) return <Skeleton />;
  if (error) return <ErrorState error={error} />;
  if (!camera) return <EmptyState />;

  return (
    <div className="page live-page">
      <h2 className="page-title">Live view</h2>
      <div className="live-layout">
        <div className="live-video-col">
          <Card title="Camera stream" className="stream-card">
            <div className="stream-wrapper">
              <VideoPlayer
                streamUrl="/camera/stream"
                recording={false}
                width={640}
                height={480}
              />
            </div>
            <div className="stream-status">
              <StatusPill variant={camera.initialised ? "success" : "danger"}>
                Camera: {camera.initialised ? "Online" : "Offline"}
              </StatusPill>
              <StatusPill variant="default">{camera.sensor}</StatusPill>
              <span className="stream-fps">{fps.toFixed(1)} fps</span>
            </div>
          </Card>
        </div>

        <div className="live-controls-col">
          <Card title="Controls">
            <div className="control-row">
              <Button variant="secondary" onClick={() => show("Snapshot not wired yet", "info")}>
                Snapshot
              </Button>
              <Button variant="danger" onClick={() => show("Record not wired yet", "info")}>
                Record
              </Button>
              <Button variant="secondary" onClick={() => show("Flash not wired yet", "info")}>
                Flash
              </Button>
            </div>
            <Slider label="Resolution" value={camera.framesize} min={0} max={15} step={1} onChange={() => {}} />
            <Slider label="JPEG quality" value={camera.quality} min={0} max={63} step={1} onChange={() => {}} />
            <Slider label="Brightness" value={0} min={-2} max={2} step={1} onChange={() => {}} />
            <Slider label="Contrast" value={0} min={-2} max={2} step={1} onChange={() => {}} />
          </Card>

          <Card title="Stream info">
            <Row label="Sensor">{camera.sensor}</Row>
            <Row label="Frame size">{camera.framesize}</Row>
            <Row label="Quality">{camera.quality}</Row>
            <Row label="JPEG capacity">{Math.round(camera.jpeg_capacity_bytes / 1024)} KB</Row>
            <ProgressBar value={50} variant="warning" label="Stream buffer" />
          </Card>
        </div>
      </div>
    </div>
  );
}

function Skeleton() {
  return (
    <div className="page">
      <h2 className="page-title">Live view</h2>
      <div className="skeleton card-skeleton" style={{ height: 400 }} />
    </div>
  );
}

function ErrorState({ error }: { error: string }) {
  return (
    <div className="page error-state">
      <div className="empty-state">
        <span className="empty-text">Error loading live view</span>
        <span className="empty-sub">{error}</span>
        <Button variant="secondary" onClick={() => window.location.reload()}>Retry</Button>
      </div>
    </div>
  );
}

function EmptyState() {
  return (
    <div className="page empty-state">
      <div className="empty-state">
        <span className="empty-text">Camera not available</span>
      </div>
    </div>
  );
}
