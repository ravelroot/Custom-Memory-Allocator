from __future__ import annotations

import sqlite3
import threading
import time
from pathlib import Path

MIGRATIONS = [
    """
    CREATE TABLE IF NOT EXISTS sessions (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        policy TEXT NOT NULL CHECK (policy IN ('first-fit', 'next-fit', 'best-fit')),
        list_order TEXT NOT NULL CHECK (list_order IN ('address', 'lifo')),
        grow_size INTEGER NOT NULL CHECK (grow_size > 0),
        mmap_threshold INTEGER NOT NULL CHECK (mmap_threshold >= 0),
        max_bytes INTEGER NOT NULL CHECK (max_bytes >= 0),
        created_at REAL NOT NULL,
        closed_at REAL
    );
    CREATE TABLE IF NOT EXISTS operations (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
        seq INTEGER NOT NULL,
        kind TEXT NOT NULL CHECK (kind IN ('alloc', 'calloc', 'realloc', 'memalign', 'free', 'write', 'policy', 'trim', 'reset', 'trace')),
        handle INTEGER,
        size INTEGER,
        offset INTEGER,
        ok INTEGER NOT NULL CHECK (ok IN (0, 1)),
        error TEXT,
        elapsed_ns INTEGER NOT NULL DEFAULT 0,
        created_at REAL NOT NULL,
        UNIQUE (session_id, seq)
    );
    CREATE INDEX IF NOT EXISTS idx_operations_session ON operations(session_id, seq);
    CREATE TABLE IF NOT EXISTS traces (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT NOT NULL UNIQUE,
        content TEXT NOT NULL,
        commands INTEGER NOT NULL CHECK (commands >= 0),
        created_at REAL NOT NULL,
        updated_at REAL NOT NULL
    );
    CREATE TABLE IF NOT EXISTS benchmark_runs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        operations INTEGER NOT NULL CHECK (operations > 0),
        seed INTEGER NOT NULL,
        min_size INTEGER NOT NULL CHECK (min_size > 0),
        max_size INTEGER NOT NULL,
        created_at REAL NOT NULL,
        CHECK (max_size >= min_size)
    );
    CREATE TABLE IF NOT EXISTS benchmark_results (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        run_id INTEGER NOT NULL REFERENCES benchmark_runs(id) ON DELETE CASCADE,
        allocator TEXT NOT NULL,
        ops_per_second REAL NOT NULL,
        seconds REAL NOT NULL,
        peak_arena_bytes INTEGER NOT NULL,
        fragmentation REAL NOT NULL,
        utilization REAL NOT NULL,
        search_steps INTEGER NOT NULL,
        failed INTEGER NOT NULL,
        UNIQUE (run_id, allocator)
    );
    CREATE INDEX IF NOT EXISTS idx_results_run ON benchmark_results(run_id);
    """,
]


class Store:
    def __init__(self, path: str | Path) -> None:
        self.path = str(path)
        if self.path != ":memory:":
            Path(self.path).parent.mkdir(parents=True, exist_ok=True)
        self.conn = sqlite3.connect(self.path, check_same_thread=False, isolation_level=None)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA foreign_keys = ON")
        self.conn.execute("PRAGMA journal_mode = WAL")
        self.conn.execute("PRAGMA busy_timeout = 5000")
        self.lock = threading.Lock()
        self.migrate()

    def migrate(self) -> None:
        with self.lock:
            self.conn.execute("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, applied_at REAL NOT NULL)")
            done = {row[0] for row in self.conn.execute("SELECT version FROM schema_migrations")}
            for version, script in enumerate(MIGRATIONS, start=1):
                if version in done:
                    continue
                self.conn.executescript("BEGIN;" + script + f"INSERT INTO schema_migrations VALUES ({version}, {time.time()});COMMIT;")

    def close(self) -> None:
        self.conn.close()

    def execute(self, sql: str, params: tuple = ()) -> sqlite3.Cursor:
        with self.lock:
            return self.conn.execute(sql, params)

    def query(self, sql: str, params: tuple = ()) -> list[dict]:
        with self.lock:
            return [dict(row) for row in self.conn.execute(sql, params).fetchall()]

    def create_session(self, sid: str, name: str, policy: str, order: str, grow: int, threshold: int, max_bytes: int) -> None:
        self.execute(
            "INSERT INTO sessions (id, name, policy, list_order, grow_size, mmap_threshold, max_bytes, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            (sid, name, policy, order, grow, threshold, max_bytes, time.time()),
        )

    def update_session_policy(self, sid: str, policy: str) -> None:
        self.execute("UPDATE sessions SET policy = ? WHERE id = ?", (policy, sid))

    def close_session(self, sid: str) -> None:
        self.execute("UPDATE sessions SET closed_at = ? WHERE id = ? AND closed_at IS NULL", (time.time(), sid))

    def sessions(self, include_closed: bool = False) -> list[dict]:
        sql = "SELECT * FROM sessions" + ("" if include_closed else " WHERE closed_at IS NULL") + " ORDER BY created_at DESC LIMIT 200"
        return self.query(sql)

    def log(self, sid: str, seq: int, kind: str, handle: int | None, size: int | None, offset: int | None, ok: bool, error: str | None, elapsed_ns: int) -> None:
        self.execute(
            "INSERT INTO operations (session_id, seq, kind, handle, size, offset, ok, error, elapsed_ns, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            (sid, seq, kind, handle, size, offset, 1 if ok else 0, error, elapsed_ns, time.time()),
        )

    def operations(self, sid: str, limit: int = 200) -> list[dict]:
        return self.query("SELECT * FROM operations WHERE session_id = ? ORDER BY seq DESC LIMIT ?", (sid, limit))

    def save_trace(self, name: str, content: str, commands: int) -> dict:
        now = time.time()
        self.execute(
            "INSERT INTO traces (name, content, commands, created_at, updated_at) VALUES (?, ?, ?, ?, ?) "
            "ON CONFLICT(name) DO UPDATE SET content = excluded.content, commands = excluded.commands, updated_at = excluded.updated_at",
            (name, content, commands, now, now),
        )
        return self.query("SELECT * FROM traces WHERE name = ?", (name,))[0]

    def traces(self) -> list[dict]:
        return self.query("SELECT id, name, commands, created_at, updated_at FROM traces ORDER BY name")

    def trace(self, tid: int) -> dict | None:
        rows = self.query("SELECT * FROM traces WHERE id = ?", (tid,))
        return rows[0] if rows else None

    def delete_trace(self, tid: int) -> bool:
        return self.execute("DELETE FROM traces WHERE id = ?", (tid,)).rowcount > 0

    def save_benchmark(self, operations: int, seed: int, min_size: int, max_size: int, results: list[dict]) -> dict:
        with self.lock:
            self.conn.execute("BEGIN")
            cur = self.conn.execute(
                "INSERT INTO benchmark_runs (operations, seed, min_size, max_size, created_at) VALUES (?, ?, ?, ?, ?)",
                (operations, seed, min_size, max_size, time.time()),
            )
            run_id = cur.lastrowid
            for r in results:
                self.conn.execute(
                    "INSERT INTO benchmark_results (run_id, allocator, ops_per_second, seconds, peak_arena_bytes, fragmentation, utilization, search_steps, failed) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
                    (run_id, r["allocator"], r["ops_per_second"], r["seconds"], r["peak_arena_bytes"], r["fragmentation"], r["utilization"], r["search_steps"], r["failed"]),
                )
            self.conn.execute("COMMIT")
        return self.benchmark(run_id)

    def benchmark(self, run_id: int) -> dict | None:
        runs = self.query("SELECT * FROM benchmark_runs WHERE id = ?", (run_id,))
        if not runs:
            return None
        run = runs[0]
        run["results"] = self.query("SELECT * FROM benchmark_results WHERE run_id = ? ORDER BY id", (run_id,))
        return run

    def benchmarks(self, limit: int = 20) -> list[dict]:
        runs = self.query("SELECT id FROM benchmark_runs ORDER BY id DESC LIMIT ?", (limit,))
        return [self.benchmark(r["id"]) for r in runs]
