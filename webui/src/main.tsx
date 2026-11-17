import React from "react";
import ReactDOM from "react-dom/client";
import { BrowserRouter, RouterProvider } from "react-router-dom";
import { ToastProvider } from "./hooks/useToast";
import { router } from "./app/routes";
import "./styles/tokens.css";
import "./styles/base.css";

ReactDOM.createRoot(document.getElementById("root")!).render(
  <React.StrictMode>
    <BrowserRouter>
      <ToastProvider>
        <RouterProvider router={router} />
      </ToastProvider>
    </BrowserRouter>
  </React.StrictMode>
);
