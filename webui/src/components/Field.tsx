import React, {
  type DetailedHTMLProps,
  type InputHTMLAttributes,
} from "react";

interface FieldProps
  extends Omit<DetailedHTMLProps<InputHTMLAttributes<HTMLInputElement>, HTMLInputElement>, "size"> {
  label: string;
  error?: string;
  showPassword?: boolean;
}

export function Field({
  label,
  error,
  showPassword = false,
  id,
  type = "text",
  ...props
}: FieldProps) {
  const [visible, setVisible] = React.useState(false);
  const inputId = id || label.toLowerCase().replace(/\s+/g, "-");
  return (
    <div className="field">
      <label htmlFor={inputId} className="field-label">
        {label}
      </label>
      <div className="field-input-wrapper">
        <input
          id={inputId}
          type={showPassword && !visible ? "password" : type}
          className={`field-input ${error ? "field-input-error" : ""}`}
          {...props}
        />
        {showPassword && (
          <button
            type="button"
            className="field-toggle"
            onClick={() => setVisible((v) => !v)}
            aria-label={visible ? "Hide password" : "Show password"}
            aria-pressed={visible}
          >
            {visible ? "🙈" : "👁"}
          </button>
        )}
      </div>
      {error && <span className="field-error">{error}</span>}
    </div>
  );
}
