import { Card } from "../components/Card";
import { Button } from "../components/Button";

interface ComingSoonPageProps {
  section: string;
}

export function ComingSoonPage({ section }: ComingSoonPageProps) {
  return (
    <div className="page">
      <h2 className="page-title">{section}</h2>
      <Card className="coming-soon-card">
        <h3 className="card-title">{section}</h3>
        <p className="coming-soon-text">Coming in Phase N — placeholder.</p>
        <Button variant="secondary" disabled>Not available yet</Button>
      </Card>
    </div>
  );
}
