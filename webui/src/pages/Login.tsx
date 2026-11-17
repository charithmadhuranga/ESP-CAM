import React from "react";
import { useNavigate } from "react-router-dom";
import { Button } from "../components/Button";
import { Field } from "../components/Field";
import { Card } from "../components/Card";
import { useToast } from "../hooks/useToast";
import { login, getSession } from "../lib/api";

export function Login() {
  const navigate = useNavigate();
  const { show } = useToast();
  const [username, setUsername] = React.useState("");
  const [password, setPassword] = React.useState("");
  const [error, setError] = React.useState<string | null>(null);
  const [loading, setLoading] = React.useState(false);

  // If already has a valid session, redirect to overview
  React.useEffect(() => {
    let cancelled = false;
    getSession().then(() => {
      if (!cancelled) navigate("/", { replace: true });
    }).catch(() => {
      // not authenticated, stay on login
    });
    return () => { cancelled = true; };
  }, [navigate]);

  const handleSubmit = async (e: React.FormEvent) => {
    e.preventDefault();
    setError(null);
    setLoading(true);
    try {
      await login({ username, password });
      show("Signed in", "success");
      navigate("/", { replace: true });
    } catch (err: any) {
      setError(err?.message ?? "Invalid username or password");
      show("Sign in failed", "error");
    } finally {
      setLoading(false);
    }
  };

  return (
    <div className="login-page">
      <Card className="login-card">
        <div className="login-wordmark">VISIONCAM PRO</div>
        <h1 className="login-title">Sign in</h1>
        <form onSubmit={handleSubmit} className="login-form">
          <Field
            label="Username"
            id="login-username"
            value={username}
            onChange={(e) => setUsername(e.target.value)}
            required
          />
          <Field
            label="Password"
            id="login-password"
            type="password"
            showPassword
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            required
          />
          {error && <div className="login-error">{error}</div>}
          <Button variant="primary" type="submit" loading={loading} disabled={loading}>
            Sign in
          </Button>
        </form>
      </Card>
    </div>
  );
}
