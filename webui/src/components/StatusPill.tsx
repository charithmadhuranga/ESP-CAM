import type { ReactNode } from "react";

interface StatusPillProps {
  children: ReactNode;
  variant?: "success" | "warning" | "danger" | "default";
}

export function StatusPill({
  children,
  variant = "default",
}: StatusPillProps) {
  return (
    <span
      className={`pill pill--${variant}`}
      aria-hidden="true"
    >
      {children}
    </span>
  );
}
