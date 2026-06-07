import { Hono } from "hono";
import { cors } from "hono/cors";
import { Database } from "bun:sqlite";
import { createHmac } from "crypto";

const SECRET = process.env.LEADERBOARD_SECRET;
if (!SECRET) throw new Error("LEADERBOARD_SECRET env var is required");

const db = new Database(process.env.DB_PATH ?? "leaderboard.db");

db.run(`
  CREATE TABLE IF NOT EXISTS scores (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    name      TEXT    NOT NULL,
    score     INTEGER NOT NULL,
    submitted_at INTEGER NOT NULL
  )
`);
db.run(`CREATE INDEX IF NOT EXISTS idx_scores_score ON scores(score DESC)`);

const insertScore = db.prepare(
  "INSERT INTO scores (name, score, submitted_at) VALUES (?, ?, ?)"
);
const topScores = db.prepare(
  "SELECT name, score, submitted_at FROM scores ORDER BY score DESC LIMIT 25"
);

function verifyHmac(name: string, score: number, timestamp: number, sig: string): boolean {
  // Reject timestamps older than 5 minutes
  if (Math.abs(Date.now() / 1000 - timestamp) > 300) return false;
  const expected = createHmac("sha256", SECRET!)
    .update(`${name}|${score}|${timestamp}`)
    .digest("hex");
  // Constant-time compare
  if (expected.length !== sig.length) return false;
  let diff = 0;
  for (let i = 0; i < expected.length; i++) diff |= expected.charCodeAt(i) ^ sig.charCodeAt(i);
  return diff === 0;
}

const app = new Hono();

app.use("/*", cors({ origin: "*" }));

app.get("/scores", (c) => {
  const rows = topScores.all();
  return c.json(rows);
});

app.post("/scores", async (c) => {
  let body: { name?: string; score?: number; timestamp?: number; hmac?: string };
  try {
    body = await c.req.json();
  } catch {
    return c.json({ error: "invalid json" }, 400);
  }

  const { name, score, timestamp, hmac } = body;

  if (typeof name !== "string" || typeof score !== "number" || typeof timestamp !== "number" || typeof hmac !== "string") {
    return c.json({ error: "missing fields" }, 400);
  }
  if (name.trim().length === 0 || name.length > 32) {
    return c.json({ error: "invalid name" }, 400);
  }
  if (!Number.isInteger(score) || score < 0 || score > 2_000_000_000) {
    return c.json({ error: "invalid score" }, 400);
  }
  if (!verifyHmac(name, score, timestamp, hmac)) {
    return c.json({ error: "invalid signature" }, 403);
  }

  insertScore.run(name.trim(), score, timestamp);
  return c.json({ ok: true }, 201);
});

export default {
  port: Number(process.env.PORT ?? 3456),
  fetch: app.fetch,
};
