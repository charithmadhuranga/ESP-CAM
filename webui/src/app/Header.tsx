import { Link, useNavigate } from "react-router-dom";
import { StatusPill } from "../components/StatusPill";
import { useToast } from "../hooks/useToast";
import { logout } from "../lib/api";

interface HeaderProps {
  wifiState: string;
  mqttState: string;
}

export function Header({ wifiState, mqttState }: HeaderProps) {
  const navigate = useNavigate();
  const { show } = useToast();

  return (
    <header className="header">
      <div className="header-left">
        <Link to="/" className="header-brand">
          VisionCam
        </Link>
      </div>
      <div className="header-pills">
        <StatusPill variant={wifiState === "Connected" ? "success" : "default"}>
          Device: {wifiState}
        </StatusPill>
        <StatusPill variant={mqttState === "Connected" ? "success" : "default"}>
          MQTT: {mqttState}
        </StatusPill>
        <StatusPill variant="default">Admin</StatusPill>
      </div>
      <div className="header-right">
        <button
          className="header-action"
          onClick={() => {
            logout().then(() => {
              show("Logged out", "info");
              navigate("/login");
            });
          }}
        >
          Sign out
        </button>
      </div>
    </header>
  );
}
