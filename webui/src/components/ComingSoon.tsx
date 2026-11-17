import type { ReactNode } from "react";
import { Card } from "./Card";
import { Button } from "./Button";

interface ComingSoonProps {
  section: string;
  detail?: ReactNode;
}

export function ComingSoon({ section, detail }: ComingSoonProps) {
  return (
    <Card className="coming-soon-card">
      <h3 className="card-title">{section}</h3>
      <p className="coming-soon-text">Coming in Phase {detail ?? "N"} — placeholder.</p>
      <Button variant="secondary" disabled>
        Not available yet
      </Button>
    </Card>
  );
}
