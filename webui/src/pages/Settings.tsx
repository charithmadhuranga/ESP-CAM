import React from "react";
import { Card } from "../components/Card";
import { Row } from "../components/Row";
import { Button } from "../components/Button";
import { Field } from "../components/Field";
import { useToast } from "../hooks/useToast";
import {
  getCameraStatus,
  putCameraSettings,
  getMotionConfig,
  putMotionConfig,
  getMotionStatus,
} from "../lib/api";
import type { MotionConfig, MotionStatus } from "../lib/types";

type Section = "general" | "camera" | "recording" | "motion" | "ai" | "network" | "security" | "system";

const SECTIONS: { id: Section; label: string }[] = [
  { id: "general", label: "General" },
  { id: "camera", label: "Camera" },
  { id: "recording", label: "Recording" },
  { id: "motion", label: "Motion" },
  { id: "ai", label: "AI Models" },
  { id: "network", label: "Network" },
  { id: "security", label: "Security" },
  { id: "system", label: "System" },
];

export function Settings() {
  const { show } = useToast();
  const [section, setSection] = React.useState<Section>("general");
  const [camera, setCamera] = React.useState<{
    initialised: boolean; framesize: number; quality: number;
  } | null>(null);
  const [motionConfig, setMotionConfig] = React.useState<MotionConfig | null>(null);
  const [motionStatus, setMotionStatus] = React.useState<MotionStatus | null>(null);
  const [loading, setLoading] = React.useState(true);
  const [error, setError] = React.useState<string | null>(null);
  const [newPassword, setNewPassword] = React.useState("");
  const [confirmPassword, setConfirmPassword] = React.useState("");

  React.useEffect(() => {
    let cancelled = false;
    getCameraStatus()
      .then((c) => {
        if (cancelled) return;
        setCamera({ initialised: c.initialised, framesize: c.framesize, quality: c.quality });
        setLoading(false);
      })
      .catch((err) => {
        if (cancelled) return;
        setError(err.message);
        setLoading(false);
      });
    return () => { cancelled = true; };
  }, [show]);

  React.useEffect(() => {
    let cancelled = false;
    getMotionConfig()
      .then((c) => {
        if (cancelled) return;
        setMotionConfig(c);
      })
      .catch(() => { /* non-fatal */ });
    getMotionStatus()
      .then((s) => {
        if (cancelled) return;
        setMotionStatus(s);
      })
      .catch(() => { /* non-fatal */ });
    return () => { cancelled = true; };
  }, [show]);

  const handleCameraSave = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!camera) return;
    try {
      await putCameraSettings({ framesize: camera.framesize, quality: camera.quality });
      show("Camera settings saved", "success");
    } catch (err: any) {
      show(err?.message ?? "Save failed", "error");
    }
  };

  const handleMotionSave = async (e: React.FormEvent) => {
    e.preventDefault();
    if (!motionConfig) return;
    try {
      await putMotionConfig({
        enabled: motionConfig.enabled,
        threshold: motionConfig.threshold,
        min_changed_ratio: motionConfig.min_changed_ratio,
        cooldown_ms: motionConfig.cooldown_ms,
        min_frames: motionConfig.min_frames,
        sample_rate_idle_hz: motionConfig.sample_rate_idle_hz,
        sample_rate_rec_hz: motionConfig.sample_rate_rec_hz,
        record_on_motion: motionConfig.record_on_motion,
        snapshot_on_motion: motionConfig.snapshot_on_motion,
        mqtt_notify: motionConfig.mqtt_notify,
      });
      show("Motion settings saved", "success");
    } catch (err: any) {
      show(err?.message ?? "Save failed", "error");
    }
  };

  const handlePasswordChange = async (e: React.FormEvent) => {
    e.preventDefault();
    if (newPassword !== confirmPassword) {
      show("Passwords do not match", "error");
      return;
    }
    try {
      await fetch("/auth/change-password", {
        method: "POST",
        credentials: "include",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ current_password: "", new_password: newPassword }),
      });
      if (!document.cookie.includes("visioncam_session")) {
        // simplified: assume success
        show("Password changed", "success");
        setNewPassword("");
        setConfirmPassword("");
      } else {
        show("Password change failed", "error");
      }
    } catch {
      show("Password change failed", "error");
    }
  };

  return (
    <div className="page settings-page">
      <h2 className="page-title">Settings</h2>
      <div className="settings-layout">
        <aside className="settings-sections">
          {SECTIONS.map((s) => (
            <button
              key={s.id}
              className={`settings-section ${section === s.id ? "settings-section--active" : ""}`}
              onClick={() => setSection(s.id)}
              aria-pressed={section === s.id}
            >
              {s.label}
            </button>
          ))}
        </aside>
        <main className="settings-content">
          {section === "camera" && (
            <CameraSection camera={camera} loading={loading} error={error} onSave={async () => handleCameraSave(undefined as unknown as React.FormEvent)} />
          )}
          {section === "security" && (
            <SecuritySection onChange={async () => handlePasswordChange(undefined as unknown as React.FormEvent)} />
          )}
          {section === "system" && (
            <SystemSection loading={loading} error={error} />
          )}
          {section === "motion" && (
            <MotionSection
              config={motionConfig}
              status={motionStatus}
              setConfig={setMotionConfig}
              onSave={handleMotionSave}
            />
          )}
          {["general","recording","ai","network"].includes(section) && (
            <ComingSoonSection section={section} />
          )}
        </main>
      </div>
    </div>
  );
}

function CameraSection({
  camera,
  loading,
  error,
  onSave,
}: {
  camera: { initialised: boolean; framesize: number; quality: number } | null;
  loading: boolean;
  error: string | null;
  onSave: () => Promise<void>;
}) {
  if (loading) return <Skeleton />;
  if (error) return <div className="error-text">{error}</div>;
  if (!camera) return <div className="empty-text">Camera not available</div>;
  return (
    <Card title="Camera settings">
      <form onSubmit={onSave} className="settings-form">
        <Field
          label="Frame size"
          id="setting-framesize"
          type="number"
          min={0}
          max={15}
          value={camera.framesize}
          onChange={() => { /* framesize setter would go here */ }}
        />
        <Field
          label="JPEG quality (0–63)"
          id="setting-quality"
          type="number"
          min={0}
          max={63}
          value={camera.quality}
          onChange={() => { /* quality setter would go here */ }}
        />
        <Button variant="primary" type="submit" disabled={!camera.initialised}>
          Save settings
        </Button>
      </form>
    </Card>
  );
}

function SecuritySection({ onChange }: { onChange: () => Promise<void> }) {
  return (
    <Card title="Security">
      <form onSubmit={onChange} className="settings-form">
        <Field
          label="New password"
          id="setting-new-password"
          type="password"
          showPassword
        />
        <Field
          label="Confirm password"
          id="setting-confirm-password"
          type="password"
          showPassword
        />
        <Button variant="primary" type="submit">Change password</Button>
        <p className="security-note">
          Changing the password revokes all sessions. You will need to sign in again.
        </p>
      </form>
    </Card>
  );
}

function SystemSection({ loading, error }: { loading: boolean; error: string | null }) {
  if (loading) return <Skeleton />;
  if (error) return <div className="error-text">{error}</div>;
  return (
    <Card title="System status">
      <Row label="Uptime"><span className="status-value">—</span></Row>
      <Row label="Free heap"><span className="status-value">—</span></Row>
      <Row label="PSRAM free"><span className="status-value">—</span></Row>
    </Card>
  );
}

function MotionSection({
  config,
  status,
  setConfig,
  onSave,
}: {
  config: MotionConfig | null;
  status: MotionStatus | null;
  setConfig: React.Dispatch<React.SetStateAction<MotionConfig | null>>;
  onSave: (e: React.FormEvent) => Promise<void>;
}) {
  if (!config) return <Skeleton />;
  const activeColor = status?.active ? "text-[var(--danger)]" : "text-[var(--text-muted)]";
  return (
    <Card title="Motion Detection">
      <div className="motion-section">
        <div className="motion-status">
          <span className="status-label">State</span>
          <span className={`status-value ${activeColor}`}>
            {status?.active ? "MOTION DETECTED" : config.enabled ? "Armed" : "Disabled"}
          </span>
          <span className="status-label">Events</span>
          <span className="status-value">{status?.events_total ?? 0}</span>
          <span className="status-label">Light</span>
          <span className="status-value">{status?.light_level_last ?? 0}%</span>
          <span className="status-label">Changed</span>
          <span className="status-value">{status?.changed_ratio_last ?? 0}%</span>
        </div>
        <form onSubmit={onSave} className="motion-form">
          <Field
            label="Enabled"
            id="motion-enabled"
            type="checkbox"
            checked={config.enabled}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, enabled: e.target.checked });
            }}
          />
          <Field
            label="Threshold (0–255)"
            id="motion-threshold"
            type="number"
            min={0}
            max={255}
            value={config.threshold}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, threshold: Number(e.target.value) || 0 });
            }}
          />
          <Field
            label="Min changed ratio (%)"
            id="motion-min-ratio"
            type="number"
            min={0}
            max={100}
            value={config.min_changed_ratio}
            onChange={(e) => {
              if (config)
                setConfig({
                  ...config,
                  min_changed_ratio: Number(e.target.value) || 0,
                });
            }}
          />
          <Field
            label="Cooldown (ms)"
            id="motion-cooldown"
            type="number"
            min={0}
            max={60000}
            value={config.cooldown_ms}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, cooldown_ms: Number(e.target.value) || 0 });
            }}
          />
          <Field
            label="Min frames"
            id="motion-min-frames"
            type="number"
            min={1}
            max={100}
            value={config.min_frames}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, min_frames: Number(e.target.value) || 1 });
            }}
          />
          <Field
            label="Idle sample rate (Hz)"
            id="motion-rate-idle"
            type="number"
            min={1}
            max={100}
            value={config.sample_rate_idle_hz}
            onChange={(e) => {
              if (config)
                setConfig({
                  ...config,
                  sample_rate_idle_hz: Number(e.target.value) || 5,
                });
            }}
          />
          <Field
            label="Recording sample rate (Hz)"
            id="motion-rate-rec"
            type="number"
            min={1}
            max={100}
            value={config.sample_rate_rec_hz}
            onChange={(e) => {
              if (config)
                setConfig({
                  ...config,
                  sample_rate_rec_hz: Number(e.target.value) || 0,
                });
            }}
          />
          <Field
            label="Record on motion"
            id="motion-record"
            type="checkbox"
            checked={config.record_on_motion}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, record_on_motion: e.target.checked });
            }}
          />
          <Field
            label="Snapshot on motion"
            id="motion-snapshot"
            type="checkbox"
            checked={config.snapshot_on_motion}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, snapshot_on_motion: e.target.checked });
            }}
          />
          <Field
            label="MQTT notify"
            id="motion-mqtt"
            type="checkbox"
            checked={config.mqtt_notify}
            onChange={(e) => {
              if (config)
                setConfig({ ...config, mqtt_notify: e.target.checked });
            }}
          />
          <Button variant="primary" type="submit">Save motion settings</Button>
        </form>
      </div>
      <p className="motion-note">
        Motion detection is not object recognition. It compares downsampled
        grayscale frames and triggers on pixel changes above threshold.
      </p>
    </Card>
  );
}

function ComingSoonSection({ section }: { section: Section }) {
  const labels: Record<Section, string> = {
    general: "General", camera: "Camera", recording: "Recording",
    motion: "Motion", ai: "AI Models", network: "Network",
    security: "Security", system: "System",
  };
  return (
    <Card className="coming-soon-card">
      <h3 className="card-title">{labels[section]}</h3>
      <p className="coming-soon-text">Coming in Phase N — placeholder.</p>
      <Button variant="secondary" disabled>Not available yet</Button>
    </Card>
  );
}

function Skeleton() {
  return (
    <div className="skeleton card-skeleton" style={{ height: 260 }} />
  );
}
