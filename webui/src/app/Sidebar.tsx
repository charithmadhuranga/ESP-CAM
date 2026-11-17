import { useLocation, useNavigate } from "react-router-dom";
import { NavItem } from "../components/Nav";

type NavEntry = { label: string; href: string };

const NAV: NavEntry[] = [
  { label: "Overview", href: "/" },
  { label: "Live View", href: "/live" },
  { label: "Recordings", href: "/recordings" },
  { label: "Snapshots", href: "/snapshots" },
  { label: "Timelapse", href: "/timelapse" },
  { label: "AI Detection", href: "/ai" },
  { label: "Storage", href: "/storage" },
  { label: "Integrations", href: "/integrations" },
  { label: "Settings", href: "/settings" },
];

export function Sidebar() {
  const location = useLocation();
  const navigate = useNavigate();
  return (
    <aside className="sidebar">
      <div className="sidebar-brand">
        VISIONCAM PRO
      </div>
      <nav className="sidebar-nav">
        {NAV.map((item) => {
          const active = location.pathname === item.href ||
            (item.href !== "/" && location.pathname.startsWith(item.href));
          return (
            <NavItem
              key={item.href}
              label={item.label}
              href={item.href}
              active={active}
              onClick={() => navigate(item.href)}
            />
          );
        })}
      </nav>
      <div className="sidebar-footer">
        <span className="sidebar-version">v0.1.0-phase1</span>
      </div>
    </aside>
  );
}
