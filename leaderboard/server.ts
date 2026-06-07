import { Hono } from "hono";
import { cors } from "hono/cors";
import { Database } from "bun:sqlite";
import { createHmac } from "crypto";

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
const SECRET = process.env.LEADERBOARD_SECRET;
if (!SECRET) throw new Error("LEADERBOARD_SECRET env var is required");

const JWT_SECRET = process.env.JWT_SECRET;
if (!JWT_SECRET) throw new Error("JWT_SECRET env var is required");

const ALLOWED_ORIGIN = process.env.ALLOWED_ORIGIN ?? "https://pinball.coltonspurgin.tech";

// ---------------------------------------------------------------------------
// Database
// ---------------------------------------------------------------------------
const db = new Database(process.env.DB_PATH ?? "leaderboard.db");

db.run(`
  CREATE TABLE IF NOT EXISTS users (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    username  TEXT    NOT NULL UNIQUE COLLATE NOCASE,
    password  TEXT    NOT NULL,
    created_at INTEGER NOT NULL
  )
`);
db.run(`
  CREATE TABLE IF NOT EXISTS scores (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    username  TEXT    NOT NULL,
    score     INTEGER NOT NULL,
    submitted_at INTEGER NOT NULL
  )
`);
db.run(`CREATE INDEX IF NOT EXISTS idx_scores_score ON scores(score DESC)`);

const findUser     = db.prepare("SELECT * FROM users WHERE username = ?");
const insertUser   = db.prepare("INSERT INTO users (username, password, created_at) VALUES (?, ?, ?)");
const insertScore  = db.prepare("INSERT INTO scores (username, score, submitted_at) VALUES (?, ?, ?)");
const topScores    = db.prepare("SELECT username, score, submitted_at FROM scores ORDER BY score DESC LIMIT 25");

// ---------------------------------------------------------------------------
// Auth helpers — simple JWT (header.payload.sig, all base64url, HMAC-SHA256)
// ---------------------------------------------------------------------------
function b64url(s: string) {
  return Buffer.from(s).toString("base64url");
}

function signJwt(payload: object): string {
  const header  = b64url(JSON.stringify({ alg: "HS256", typ: "JWT" }));
  const body    = b64url(JSON.stringify(payload));
  const sig     = createHmac("sha256", JWT_SECRET!).update(`${header}.${body}`).digest("base64url");
  return `${header}.${body}.${sig}`;
}

function verifyJwt(token: string): { username: string; exp: number } | null {
  try {
    const [header, body, sig] = token.split(".");
    const expected = createHmac("sha256", JWT_SECRET!).update(`${header}.${body}`).digest("base64url");
    if (sig !== expected) return null;
    const payload = JSON.parse(Buffer.from(body, "base64url").toString());
    if (payload.exp < Date.now() / 1000) return null;
    return payload;
  } catch { return null; }
}

function verifyHmac(username: string, score: number, timestamp: number, sig: string): boolean {
  if (Math.abs(Date.now() / 1000 - timestamp) > 300) return false;
  const expected = createHmac("sha256", SECRET!)
    .update(`${username}|${score}|${timestamp}`)
    .digest("hex");
  if (expected.length !== sig.length) return false;
  let diff = 0;
  for (let i = 0; i < expected.length; i++) diff |= expected.charCodeAt(i) ^ sig.charCodeAt(i);
  return diff === 0;
}

// ---------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------
const app = new Hono();

app.use("/*", cors({ origin: ALLOWED_ORIGIN }));

// POST /register
app.post("/register", async (c) => {
  const origin = c.req.header("origin") ?? "";
  if (origin && origin !== ALLOWED_ORIGIN) return c.json({ error: "forbidden" }, 403);

  const { username, password } = await c.req.json().catch(() => ({})) as any;
  if (typeof username !== "string" || typeof password !== "string")
    return c.json({ error: "missing fields" }, 400);
  if (!/^[a-zA-Z0-9_-]{2,20}$/.test(username))
    return c.json({ error: "username must be 2-20 chars, letters/numbers/_ only" }, 400);
  if (password.length < 4)
    return c.json({ error: "password must be at least 4 characters" }, 400);

  const existing = findUser.get(username);
  if (existing) return c.json({ error: "username taken" }, 409);

  const hash = await Bun.password.hash(password);
  insertUser.run(username, hash, Math.floor(Date.now() / 1000));

  const token = signJwt({ username, exp: Math.floor(Date.now() / 1000) + 60 * 60 * 24 * 30 });
  return c.json({ token, username }, 201);
});

// POST /login
app.post("/login", async (c) => {
  const origin = c.req.header("origin") ?? "";
  if (origin && origin !== ALLOWED_ORIGIN) return c.json({ error: "forbidden" }, 403);

  const { username, password } = await c.req.json().catch(() => ({})) as any;
  if (typeof username !== "string" || typeof password !== "string")
    return c.json({ error: "missing fields" }, 400);

  const user = findUser.get(username) as any;
  if (!user) return c.json({ error: "invalid username or password" }, 401);

  const valid = await Bun.password.verify(password, user.password);
  if (!valid) return c.json({ error: "invalid username or password" }, 401);

  const token = signJwt({ username, exp: Math.floor(Date.now() / 1000) + 60 * 60 * 24 * 30 });
  return c.json({ token, username });
});

// GET /scores
app.get("/scores", (c) => {
  return c.json(topScores.all());
});

// POST /scores
app.post("/scores", async (c) => {
  const origin = c.req.header("origin") ?? "";
  if (origin && origin !== ALLOWED_ORIGIN) return c.json({ error: "forbidden" }, 403);

  // Verify JWT
  const auth = c.req.header("authorization") ?? "";
  const token = auth.startsWith("Bearer ") ? auth.slice(7) : "";
  const user = verifyJwt(token);
  if (!user) return c.json({ error: "unauthorized" }, 401);

  const { score, timestamp, hmac } = await c.req.json().catch(() => ({})) as any;
  if (typeof score !== "number" || typeof timestamp !== "number" || typeof hmac !== "string")
    return c.json({ error: "missing fields" }, 400);
  if (!Number.isInteger(score) || score < 0 || score > 2_000_000_000)
    return c.json({ error: "invalid score" }, 400);
  if (!verifyHmac(user.username, score, timestamp, hmac))
    return c.json({ error: "invalid signature" }, 403);

  insertScore.run(user.username, score, timestamp);
  return c.json({ ok: true }, 201);
});

export default {
  port: Number(process.env.PORT ?? 3456),
  fetch: app.fetch,
};
