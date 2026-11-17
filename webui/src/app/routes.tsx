import { createBrowserRouter } from "react-router-dom";
import { Login } from "../pages/Login";
import { AppShell } from "./AppShell";
import { Overview } from "../pages/Overview";
import { LiveView } from "../pages/LiveView";
import { Settings } from "../pages/Settings";
import { ComingSoonPage } from "../pages/ComingSoon";

export const router = createBrowserRouter([
  {
    path: "/login",
    element: <Login />,
  },
  {
    path: "/",
    element: <AppShell />,
    children: [
      { index: true, element: <Overview /> },
      { path: "live", element: <LiveView /> },
      { path: "recordings", element: <ComingSoonPage section="Recordings" /> },
      { path: "snapshots", element: <ComingSoonPage section="Snapshots" /> },
      { path: "timelapse", element: <ComingSoonPage section="Timelapse" /> },
      { path: "ai", element: <ComingSoonPage section="AI Detection" /> },
      { path: "storage", element: <ComingSoonPage section="Storage" /> },
      { path: "integrations", element: <ComingSoonPage section="Integrations" /> },
      { path: "settings", element: <Settings /> },
    ],
  },
]);
