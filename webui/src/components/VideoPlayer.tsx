import React from "react";

interface VideoPlayerProps {
  streamUrl: string;
  recording: boolean;
  fps?: number;
  width?: number;
  height?: number;
}

export function VideoPlayer({
  streamUrl,
  recording = false,
  fps,
  width = 640,
  height = 480,
}: VideoPlayerProps) {
  const [connected, setConnected] = React.useState(false);
  const [error, setError] = React.useState<string | null>(null);

  React.useEffect(() => {
    let cancelled = false;
    let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
    let backoff = 1000;

    const start = () => {
      if (cancelled) return;
      const img = new Image();
      const url = `${streamUrl}?t=${Date.now()}`;
      img.onload = () => {
        if (cancelled) return;
        setConnected(true);
        setError(null);
        backoff = 1000;
      };
      img.onerror = () => {
        if (cancelled) return;
        setConnected(false);
        setError("Stream unavailable — reconnecting…");
        reconnectTimer = setTimeout(() => {
          if (!cancelled) {
            backoff = Math.min(backoff * 2, 8000);
            start();
          }
        }, backoff);
      };
      img.src = url;
      // detach handlers
      img.onload = img.onerror = null as any;
    };

    start();
    return () => {
      cancelled = true;
      if (reconnectTimer) clearTimeout(reconnectTimer);
    };
  }, [streamUrl]);

  return (
    <div className="video-player">
      {error && !connected ? (
        <div className="video-placeholder">
          <span className="video-caption">{error}</span>
        </div>
      ) : (
        <>
          <img
            src={connected ? `${streamUrl}?t=${Date.now()}` : ""}
            alt="Live stream"
            className="video-img"
            width={width}
            height={height}
          />
          {recording && <span className="rec-pill">REC</span>}
          {fps !== undefined && <span className="video-fps">{fps.toFixed(1)} fps</span>}
        </>
      )}
    </div>
  );
}
