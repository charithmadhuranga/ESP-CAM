import React, { type ReactNode } from "react";
import { Button } from "./Button";

interface ConfirmDialogProps {
  open: boolean;
  onClose: () => void;
  onConfirm: () => void;
  title: string;
  message: ReactNode;
  confirmLabel?: string;
  confirmType?: "danger" | "secondary";
  typedConfirm?: string; // if set, user must type this exact word
}

export function ConfirmDialog({
  open,
  onClose,
  onConfirm,
  title,
  message,
  confirmLabel = "Confirm",
  confirmType = "secondary",
  typedConfirm,
}: ConfirmDialogProps) {
  const [typed, setTyped] = React.useState("");
  const canConfirm = typedConfirm ? typed === typedConfirm : true;

  if (!open) return null;

  return (
    <>
      <div className="backdrop" onClick={onClose} />
      <div className="dialog">
        <h2 className="dialog-title">{title}</h2>
        <div className="dialog-message">{message}</div>
        {typedConfirm && (
          <div className="dialog-typed">
            <label className="dialog-typed-label">
              Type "{typedConfirm}" to confirm:
            </label>
            <input
              className="dialog-typed-input"
              value={typed}
              onChange={(e) => setTyped(e.target.value)}
              disabled={typed === typedConfirm}
              autoFocus
            />
          </div>
        )}
        <div className="dialog-actions">
          <Button variant="secondary" onClick={onClose}>
            Cancel
          </Button>
          <Button
            variant={confirmType}
            onClick={onConfirm}
            disabled={!canConfirm}
          >
            {confirmLabel}
          </Button>
        </div>
      </div>
    </>
  );
}
