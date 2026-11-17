import { useNavigate } from "react-router-dom";

interface NavItemProps {
  label: string;
  href: string;
  active: boolean;
  onClick?: () => void;
}

export function NavItem({ label, href, active, onClick }: NavItemProps) {
  const navigate = useNavigate();
  const handleClick = () => {
    onClick?.();
    navigate(href);
  };
  return (
    <button
      className={`nav-item ${active ? "nav-item--active" : ""}`}
      onClick={handleClick}
      aria-current={active ? "page" : undefined}
    >
      {label}
    </button>
  );
}
