import { Outlet, useLocation } from "react-router-dom";
import { Sidebar } from "./Sidebar";
import { Header } from "./Header";
import { Footer } from "./Footer";

export function AppShell() {
  const location = useLocation();
  // Login page has no shell
  if (location.pathname === "/login") {
    return <Outlet />;
  }
  return (
    <div className="appshell">
      <aside className="appshell-sidebar">
        <Sidebar />
      </aside>
      <div className="appshell-main">
        <Header wifiState="Connected" mqttState="Not configured" />
        <main className="appshell-content">
          <Outlet />
        </main>
        <Footer firmware="v0.1.0-phase1" sdState="Mounted" wifiState="Connected" />
      </div>
    </div>
  );
}
