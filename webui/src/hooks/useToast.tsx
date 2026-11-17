import { createContext, useContext, useCallback, useState } from "react";

export interface Toast {
  id: string;
  message: string;
  variant: "success" | "error" | "warning" | "info";
  duration?: number;
}

interface ToastContextValue {
  toasts: Toast[];
  show: (message: string, variant?: Toast["variant"], duration?: number) => void;
  dismiss: (id: string) => void;
}

const ToastContext = createContext<ToastContextValue | null>(null);

export function ToastProvider({ children }: { children: React.ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);

  const show = useCallback(
    (message: string, variant: Toast["variant"] = "info", duration = 4000) => {
      const id = crypto.randomUUID();
      setToasts((ts) => [...ts, { id, message, variant, duration }]);
      if (duration > 0) {
        setTimeout(() => setToasts((ts) => ts.filter((t) => t.id !== id)), duration);
      }
      return id;
    },
    []
  );

  const dismiss = useCallback((id: string) => {
    setToasts((ts) => ts.filter((t) => t.id !== id));
  }, []);

  const value = { toasts, show, dismiss };

  return (
    <ToastContext.Provider value={value}>
      {children}
      <ToastList />
    </ToastContext.Provider>
  );
}

export function useToast() {
  const ctx = useContext(ToastContext);
  if (!ctx) throw new Error("useToast must be used within a ToastProvider");
  return ctx;
}

function ToastList() {
  const { toasts, dismiss } = useToast();
  return (
    <div className="toast-container">
      {toasts.map((t) => (
        <div
          key={t.id}
          className={`toast toast--${t.variant}`}
          onClick={() => dismiss(t.id)}
        >
          {t.message}
        </div>
      ))}
    </div>
  );
}
