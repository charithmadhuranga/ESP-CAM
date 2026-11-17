import type { ReactNode } from "react";

interface RowProps {
  label: ReactNode;
  value?: ReactNode;
  children?: ReactNode;
}

export function Row({ label, value, children }: RowProps) {
  return (
    <div className="row">
      <span className="row-label">{label}</span>
      <span className="row-value">{children ?? value}</span>
    </div>
  );
}
