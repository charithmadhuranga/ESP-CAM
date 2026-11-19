import { createBrowserRouter } from "react-router-dom";
import { Login } from "../pages/Login";
import { AppShell } from "./AppShell";
import { Overview } from "../pages/Overview";
import { LiveView } from "../pages/LiveView";
import { Settings } from "../pages/Settings";
import { Snapshots } from "../pages/Snapshots";
import { Storage } from "../pages/Storage";
import { Recordings } from "../pages/Recordings";
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
      { path: "recordings", element: <Recordings /> },
      { path: "snapshots", element: <Snapshots /> },
      { path: "timelapse", element: <ComingSoonPage section="Timelapse" /> },
      { path: "ai", element: <ComingSoonPage section="AI Detection" /> },
      { path: "storage", element: <Storage /> },
      { path: "integrations", element: <ComingSoonPage section="Integrations" /> },
      { path: "settings", element: <Settings /> },
    ],
  },
]);
