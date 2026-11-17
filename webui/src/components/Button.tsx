import type { ButtonHTMLAttributes } from "react";

export type ButtonVariant = "primary" | "secondary" | "danger";

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: ButtonVariant;
  loading?: boolean;
}

export function Button({
  variant = "primary",
  loading = false,
  disabled,
  children,
  className,
  ...props
}: ButtonProps) {
  const base =
    "inline-flex items-center justify-center gap-8 rounded-[8px] font-medium text-[13px] " +
    "transition-colors disabled:opacity-50 disabled:pointer-events-none";
  const styles: Record<ButtonVariant, string> = {
    primary: `bg-[var(--accent)] text-[var(--bg)] px-3 py-[7px]`,
    secondary: `bg-[var(--elevated)] border border-[var(--border)] text-[var(--text)] px-3 py-[6px]`,
    danger: `bg-[var(--bg)] border border-[var(--danger-fill)] text-[var(--danger)] px-3 py-[6px]`,
  };
  return (
    <button
      className={`${base} ${styles[variant]} ${className ?? ""}`}
      disabled={disabled || loading}
      {...props}
    >
      {loading ? (
        <span className="loading-dots" aria-hidden="true" />
      ) : (
        children
      )}
    </button>
  );
}
