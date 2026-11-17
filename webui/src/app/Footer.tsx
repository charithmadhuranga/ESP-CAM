interface FooterProps {
  firmware: string;
  sdState: string;
  wifiState: string;
}

export function Footer({ firmware, sdState, wifiState }: FooterProps) {
  return (
    <footer className="footer">
      <div className="footer-item">Firmware: {firmware}</div>
      <div className="footer-divider" aria-hidden="true" />
      <div className="footer-item">SD: {sdState}</div>
      <div className="footer-divider" aria-hidden="true" />
      <div className="footer-item">Wi-Fi: {wifiState}</div>
    </footer>
  );
}
