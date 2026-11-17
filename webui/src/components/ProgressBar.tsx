interface ProgressBarProps {
  value: number; // 0..100
  variant?: "default" | "success" | "warning" | "danger";
  label?: string;
}

export function ProgressBar({
  value,
  variant = "default",
  label,
}: ProgressBarProps) {
  const fillColor = `var(--${variant === "default" ? "accent" : variant})`;
  return (
    <div className="progress">
      {label && <span className="progress-label">{label}</span>}
      <div className="progress-track">
        <div
          className="progress-fill"
          style={{
            width: Math.max(0, Math.min(100, value)),
            backgroundColor: fillColor,
          }}
          role="progressbar"
          aria-valuenow={value}
          aria-valuemin={0}
          aria-valuemax={100}
        />
      </div>
    </div>
  );
}
