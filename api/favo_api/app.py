from __future__ import annotations

import asyncio
import os
import time
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Literal

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse, JSONResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from . import __version__
from .binding import POLICIES, Heap, HeapError, Library
from .sessions import Session, SessionManager, TraceError, generate_trace, parse_trace, run_trace
from .store import Store

Policy = Literal["first-fit", "next-fit", "best-fit"]
Order = Literal["address", "lifo"]
MAX_BLOCK = 8 * 1024 * 1024


class SessionIn(BaseModel):
    name: str = Field("Sessão", min_length=1, max_length=80)
    policy: Policy = "first-fit"
    order: Order = "address"
    grow_size: int = Field(64 * 1024, ge=16 * 1024, le=16 * 1024 * 1024)
    mmap_threshold: int = Field(32 * 1024, ge=0, le=64 * 1024 * 1024)
    max_bytes: int = Field(16 * 1024 * 1024, ge=64 * 1024, le=256 * 1024 * 1024)


class AllocIn(BaseModel):
    size: int = Field(..., ge=0, le=MAX_BLOCK)
    label: str = Field("", max_length=40)


class CallocIn(BaseModel):
    count: int = Field(..., ge=0, le=1024 * 1024)
    size: int = Field(..., ge=0, le=MAX_BLOCK)
    label: str = Field("", max_length=40)


class MemalignIn(BaseModel):
    alignment: int = Field(..., ge=1, le=1024 * 1024)
    size: int = Field(..., ge=0, le=MAX_BLOCK)
    label: str = Field("", max_length=40)


class ReallocIn(BaseModel):
    handle: int
    size: int = Field(..., ge=1, le=MAX_BLOCK)


class HandleIn(BaseModel):
    handle: int


class WriteIn(BaseModel):
    handle: int
    text: str = Field(..., max_length=65536)


class PolicyIn(BaseModel):
    policy: Policy


class TraceRunIn(BaseModel):
    content: str = Field(..., max_length=2_000_000)


class TraceIn(BaseModel):
    name: str = Field(..., min_length=1, max_length=80, pattern=r"^[\w .\-]+$")
    content: str = Field(..., max_length=2_000_000)


class GenerateIn(BaseModel):
    operations: int = Field(500, ge=1, le=50000)
    seed: int = Field(42, ge=0)
    min_size: int = Field(16, ge=1, le=MAX_BLOCK)
    max_size: int = Field(2048, ge=1, le=MAX_BLOCK)


class CompareIn(BaseModel):
    content: str = Field(..., max_length=2_000_000)
    grow_size: int = Field(64 * 1024, ge=16 * 1024, le=16 * 1024 * 1024)
    mmap_threshold: int = Field(32 * 1024, ge=0, le=64 * 1024 * 1024)


class BenchIn(BaseModel):
    operations: int = Field(200_000, ge=1000, le=5_000_000)
    seed: int = Field(42, ge=0)
    min_size: int = Field(16, ge=1, le=1024 * 1024)
    max_size: int = Field(4096, ge=1, le=1024 * 1024)
    include_system: bool = True


class Hub:
    def __init__(self) -> None:
        self.clients: dict[str, set[WebSocket]] = {}

    def add(self, sid: str, ws: WebSocket) -> None:
        self.clients.setdefault(sid, set()).add(ws)

    def discard(self, sid: str, ws: WebSocket) -> None:
        self.clients.get(sid, set()).discard(ws)

    async def publish(self, session: Session) -> None:
        targets = list(self.clients.get(session.id, ()))
        if not targets:
            return
        payload = {"type": "snapshot", "data": session.snapshot()}
        for ws in targets:
            try:
                await ws.send_json(payload)
            except Exception:
                self.discard(session.id, ws)


def create_app(library: Library | None = None, database: str | None = None, web_dir: str | None = None) -> FastAPI:
    lib = library or Library()
    store = Store(database or os.environ.get("FAVO_DATABASE", "data/favo.db"))
    manager = SessionManager(lib, store, int(os.environ.get("FAVO_MAX_SESSIONS", "32")))
    hub = Hub()

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        yield
        manager.close_all()
        store.close()

    app = FastAPI(
        title="Favo API",
        version=__version__,
        description="API do alocador de memória Favo: sessões de heap isoladas, alocação, liberação, inspeção de blocos, lista livre, árvore rubro-negra, traces e benchmarks.",
        lifespan=lifespan,
    )
    origins = [o for o in os.environ.get("FAVO_CORS", "").split(",") if o]
    if origins:
        app.add_middleware(CORSMiddleware, allow_origins=origins, allow_methods=["*"], allow_headers=["*"])
    app.state.library = lib
    app.state.store = store
    app.state.manager = manager

    @app.exception_handler(HeapError)
    async def heap_error(_, exc: HeapError):
        return JSONResponse({"detail": str(exc), "code": exc.code}, status_code=409)

    def session_or_404(sid: str) -> Session:
        session = manager.get(sid)
        if not session:
            raise HTTPException(404, "sessão não encontrada")
        return session

    def allocation_or_404(session: Session, handle: int):
        alloc = session.allocations.get(handle)
        if not alloc:
            raise HTTPException(404, f"bloco #{handle} não existe nesta sessão")
        return alloc

    def offset_of(session: Session, address: int) -> int | None:
        for arena in session.heap.arenas():
            if arena["base"] <= address < arena["base"] + arena["length"]:
                return address - arena["base"]
        return None

    async def mutate(session: Session, kind: str, action, handle: int | None = None, size: int | None = None):
        start = time.perf_counter_ns()
        with session.lock:
            seq = session.next_seq()
            try:
                result = action()
            except HeapError as exc:
                store.log(session.id, seq, kind, handle, size, None, False, str(exc), time.perf_counter_ns() - start)
                await hub.publish(session)
                raise
            elapsed = time.perf_counter_ns() - start
            address = getattr(result, "address", None)
            store.log(session.id, seq, kind, getattr(result, "handle", handle), size, offset_of(session, address) if address else None, True, None, elapsed)
        await hub.publish(session)
        return result

    def alloc_out(session: Session, alloc) -> dict:
        return {
            "handle": alloc.handle,
            "address": alloc.address,
            "address_hex": hex(alloc.address),
            "size": alloc.size,
            "usable": session.heap.usable_size(alloc.address),
            "label": alloc.label,
            "kind": alloc.kind,
            "aligned_to": alloc.address & -alloc.address,
        }

    @app.get("/api/health", tags=["sistema"])
    def health():
        return {"status": "ok", "version": __version__}

    @app.get("/api/info", tags=["sistema"])
    def info():
        return {
            "version": __version__,
            "library_version": lib.version,
            "page_size": lib.page_size,
            "alignment": 16,
            "header": 16,
            "footer": 8,
            "min_block": 64,
            "policies": list(POLICIES),
            "orders": ["address", "lifo"],
            "sessions": len(manager.sessions),
        }

    @app.get("/api/sessions", tags=["sessões"])
    def list_sessions():
        return [s.describe() for s in manager.sessions.values()]

    @app.get("/api/sessions/history", tags=["sessões"])
    def session_history():
        return store.sessions(include_closed=True)

    @app.post("/api/sessions", status_code=201, tags=["sessões"])
    def create_session(body: SessionIn):
        try:
            session = manager.create(body.name, body.policy, body.order, body.grow_size, body.mmap_threshold, body.max_bytes)
        except OverflowError as exc:
            raise HTTPException(429, str(exc)) from exc
        return session.describe()

    @app.get("/api/sessions/{sid}", tags=["sessões"])
    def get_session(sid: str):
        return session_or_404(sid).describe()

    @app.delete("/api/sessions/{sid}", status_code=204, tags=["sessões"])
    def delete_session(sid: str):
        if not manager.remove(sid):
            raise HTTPException(404, "sessão não encontrada")

    @app.get("/api/sessions/{sid}/heap", tags=["inspeção"])
    def heap_snapshot(sid: str, limit: int = 4096):
        return session_or_404(sid).snapshot(max(1, min(limit, 20000)))

    @app.get("/api/sessions/{sid}/operations", tags=["inspeção"])
    def operations(sid: str, limit: int = 200):
        session_or_404(sid)
        return store.operations(sid, max(1, min(limit, 2000)))

    @app.post("/api/sessions/{sid}/alloc", status_code=201, tags=["memória"])
    async def alloc(sid: str, body: AllocIn):
        s = session_or_404(sid)
        a = await mutate(s, "alloc", lambda: s.register(s.heap.alloc(body.size), body.size, body.label, "malloc"), size=body.size)
        return alloc_out(s, a)

    @app.post("/api/sessions/{sid}/calloc", status_code=201, tags=["memória"])
    async def calloc(sid: str, body: CallocIn):
        s = session_or_404(sid)
        total = body.count * body.size
        if total > MAX_BLOCK:
            raise HTTPException(422, "tamanho total acima do limite de 8 MiB")
        a = await mutate(s, "calloc", lambda: s.register(s.heap.calloc(body.count, body.size), total, body.label, "calloc"), size=total)
        return alloc_out(s, a)

    @app.post("/api/sessions/{sid}/memalign", status_code=201, tags=["memória"])
    async def memalign(sid: str, body: MemalignIn):
        s = session_or_404(sid)
        if body.alignment & (body.alignment - 1):
            raise HTTPException(422, "o alinhamento precisa ser potência de 2")
        a = await mutate(s, "memalign", lambda: s.register(s.heap.memalign(body.alignment, body.size), body.size, body.label, f"memalign {body.alignment}"), size=body.size)
        return alloc_out(s, a)

    @app.post("/api/sessions/{sid}/realloc", tags=["memória"])
    async def realloc(sid: str, body: ReallocIn):
        s = session_or_404(sid)
        current = allocation_or_404(s, body.handle)

        def action():
            current.address = s.heap.realloc(current.address, body.size)
            current.size = body.size
            return current

        a = await mutate(s, "realloc", action, body.handle, body.size)
        return alloc_out(s, a)

    @app.post("/api/sessions/{sid}/free", tags=["memória"])
    async def free(sid: str, body: HandleIn):
        s = session_or_404(sid)
        current = allocation_or_404(s, body.handle)

        def action():
            s.heap.free(current.address)
            s.allocations.pop(current.handle, None)
            return None

        await mutate(s, "free", action, body.handle, current.size)
        return {"freed": body.handle}

    @app.post("/api/sessions/{sid}/free-all", tags=["memória"])
    async def free_all(sid: str):
        s = session_or_404(sid)

        def action():
            for a in list(s.allocations.values()):
                s.heap.free(a.address)
                s.allocations.pop(a.handle, None)

        await mutate(s, "free", action)
        return {"ok": True}

    @app.post("/api/sessions/{sid}/write", tags=["memória"])
    async def write(sid: str, body: WriteIn):
        s = session_or_404(sid)
        current = allocation_or_404(s, body.handle)
        data = body.text.encode()
        usable = s.heap.usable_size(current.address)
        if len(data) > usable:
            raise HTTPException(422, f"o texto tem {len(data)} bytes e o bloco comporta {usable}")
        payload = data + b"\0" if len(data) < usable else data
        await mutate(s, "write", lambda: s.heap.write(current.address, payload), body.handle, len(data))
        return {"handle": body.handle, "written": len(data), "usable": usable}

    @app.get("/api/sessions/{sid}/read/{handle}", tags=["memória"])
    def read(sid: str, handle: int, length: int = 256):
        s = session_or_404(sid)
        current = allocation_or_404(s, handle)
        usable = s.heap.usable_size(current.address)
        n = max(0, min(length, usable, 65536))
        with s.lock:
            raw = s.heap.read(current.address, n)
        return {"handle": handle, "length": n, "usable": usable, "hex": raw.hex(" "), "text": raw.split(b"\0", 1)[0].decode(errors="replace")}

    @app.put("/api/sessions/{sid}/policy", tags=["sessões"])
    async def set_policy(sid: str, body: PolicyIn):
        s = session_or_404(sid)
        await mutate(s, "policy", lambda: s.heap.set_policy(POLICIES[body.policy]))
        store.update_session_policy(sid, body.policy)
        return s.describe()

    @app.post("/api/sessions/{sid}/trim", tags=["memória"])
    async def trim(sid: str):
        s = session_or_404(sid)
        released = await mutate(s, "trim", s.heap.trim)
        return {"released": released}

    @app.post("/api/sessions/{sid}/reset", tags=["memória"])
    async def reset(sid: str):
        s = session_or_404(sid)

        def action():
            s.heap.reset()
            s.allocations.clear()

        await mutate(s, "reset", action)
        return s.describe()

    @app.post("/api/sessions/{sid}/trace", tags=["traces"])
    async def trace_in_session(sid: str, body: TraceRunIn):
        s = session_or_404(sid)
        try:
            ops = parse_trace(body.content)
        except TraceError as exc:
            raise HTTPException(422, str(exc)) from exc

        def action():
            result = run_trace(s.heap, ops)
            for ident, address in result.pop("live").items():
                s.register(address, s.heap.usable_size(address), f"trace {ident}", "trace")
            return result

        return await mutate(s, "trace", action, size=len(ops))

    @app.get("/api/traces", tags=["traces"])
    def list_traces():
        return store.traces()

    @app.post("/api/traces", status_code=201, tags=["traces"])
    def save_trace(body: TraceIn):
        try:
            ops = parse_trace(body.content)
        except TraceError as exc:
            raise HTTPException(422, str(exc)) from exc
        return store.save_trace(body.name, body.content, len(ops))

    @app.get("/api/traces/{tid}", tags=["traces"])
    def get_trace(tid: int):
        t = store.trace(tid)
        if not t:
            raise HTTPException(404, "trace não encontrado")
        return t

    @app.delete("/api/traces/{tid}", status_code=204, tags=["traces"])
    def delete_trace(tid: int):
        if not store.delete_trace(tid):
            raise HTTPException(404, "trace não encontrado")

    @app.post("/api/traces/generate", tags=["traces"])
    def generate(body: GenerateIn):
        if body.max_size < body.min_size:
            raise HTTPException(422, "max_size precisa ser maior ou igual a min_size")
        content = generate_trace(body.operations, body.seed, body.min_size, body.max_size)
        return {"content": content, "commands": content.count("\n")}

    @app.post("/api/compare", tags=["traces"])
    def compare(body: CompareIn):
        try:
            ops = parse_trace(body.content)
        except TraceError as exc:
            raise HTTPException(422, str(exc)) from exc
        out = []
        for name, policy in POLICIES.items():
            heap = Heap(lib, policy, 0, body.grow_size, body.mmap_threshold, 256 * 1024 * 1024)
            try:
                started = time.perf_counter()
                result = run_trace(heap, ops)
                result.pop("live")
                elapsed = time.perf_counter() - started
                blocks, total = heap.blocks(2000)
                out.append({
                    "policy": name,
                    "seconds": elapsed,
                    "result": result,
                    "stats": heap.stats(),
                    "check": heap.check(),
                    "arenas": heap.arenas(),
                    "blocks": blocks,
                    "total_blocks": total,
                })
            finally:
                heap.close()
        return {"commands": len(ops), "policies": out}

    @app.post("/api/benchmarks", status_code=201, tags=["benchmarks"])
    def run_benchmark(body: BenchIn):
        if body.max_size < body.min_size:
            raise HTTPException(422, "max_size precisa ser maior ou igual a min_size")
        results = [lib.bench(p, body.operations, body.seed, body.min_size, body.max_size) for p in POLICIES.values()]
        if body.include_system:
            results.append(lib.bench(None, body.operations, body.seed, body.min_size, body.max_size))
        return store.save_benchmark(body.operations, body.seed, body.min_size, body.max_size, results)

    @app.get("/api/benchmarks", tags=["benchmarks"])
    def list_benchmarks(limit: int = 20):
        return store.benchmarks(max(1, min(limit, 100)))

    @app.get("/api/benchmarks/{run_id}", tags=["benchmarks"])
    def get_benchmark(run_id: int):
        run = store.benchmark(run_id)
        if not run:
            raise HTTPException(404, "benchmark não encontrado")
        return run

    @app.websocket("/ws/sessions/{sid}")
    async def session_socket(ws: WebSocket, sid: str):
        session = manager.get(sid)
        await ws.accept()
        if not session:
            await ws.send_json({"type": "error", "detail": "sessão não encontrada"})
            await ws.close(code=4404)
            return
        hub.add(sid, ws)
        try:
            await ws.send_json({"type": "snapshot", "data": session.snapshot()})
            while True:
                message = await ws.receive_text()
                if message == "ping":
                    await ws.send_json({"type": "pong"})
                elif message == "refresh":
                    await ws.send_json({"type": "snapshot", "data": session.snapshot()})
        except (WebSocketDisconnect, RuntimeError, asyncio.CancelledError):
            pass
        finally:
            hub.discard(sid, ws)

    static = Path(web_dir or os.environ.get("FAVO_WEB_DIR", Path(__file__).resolve().parent.parent.parent / "web" / "dist"))
    if static.is_dir() and (static / "index.html").is_file():
        app.mount("/assets", StaticFiles(directory=static / "assets", check_dir=False), name="assets")

        @app.get("/", include_in_schema=False)
        def index():
            return FileResponse(static / "index.html")

        @app.get("/{path:path}", include_in_schema=False)
        def spa(path: str):
            candidate = (static / path).resolve()
            if candidate.is_file() and static.resolve() in candidate.parents:
                return FileResponse(candidate)
            if path.startswith("api/") or path.startswith("ws/"):
                raise HTTPException(404, "rota não encontrada")
            return FileResponse(static / "index.html")

    return app
