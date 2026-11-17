import type { ReactNode } from "react";

interface StatCardProps {
  label: string;
  value: ReactNode;
  variant?: "default" | "success" | "warning" | "danger";
}

export function StatCard({
  label,
  value,
  variant = "default",
}: StatCardProps) {
  return (
    <div className={`stat-card stat-card--${variant}`}>
      <span className="stat-label">{label}</span>
      <span className="stat-value">{value}</span>
    </div>
  );
}
