from __future__ import annotations

import random
import threading
import time
import uuid
from dataclasses import dataclass, field

from .binding import ORDERS, POLICIES, POLICY_NAMES, Heap, HeapError, Library
from .store import Store

HEADER = 16


@dataclass
class Allocation:
    handle: int
    address: int
    size: int
    label: str
    kind: str
    created_at: float = field(default_factory=time.time)


class TraceError(ValueError):
    pass


def parse_trace(text: str, limit: int = 50000) -> list[tuple]:
    ops = []
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line[0] in "#;":
            continue
        parts = line.split()
        kind = parts[0].lower()
        try:
            values = [int(p) for p in parts[1:]]
        except ValueError as exc:
            raise TraceError(f"linha {number}: número inválido") from exc
        expected = {"a": 2, "r": 2, "c": 3, "m": 3, "f": 1}
        if kind not in expected or len(values) != expected[kind]:
            raise TraceError(f"linha {number}: comando inválido '{line}'")
        if values[0] < 0 or values[0] > 1_000_000 or any(v < 0 for v in values):
            raise TraceError(f"linha {number}: valores fora do intervalo")
        ops.append((kind, *values))
        if len(ops) > limit:
            raise TraceError(f"o trace passa do limite de {limit} comandos")
    return ops


def generate_trace(operations: int, seed: int, min_size: int, max_size: int) -> str:
    rng = random.Random(seed)
    slots = max(8, operations // 4)
    live: set[int] = set()
    lines = []
    for _ in range(operations):
        slot = rng.randrange(slots)
        size = rng.randint(min_size, max_size)
        if slot not in live:
            lines.append(f"a {slot} {size}")
            live.add(slot)
        elif rng.random() < 0.75:
            lines.append(f"f {slot}")
            live.discard(slot)
        else:
            lines.append(f"r {slot} {size}")
    return "\n".join(lines) + "\n"


def run_trace(heap: Heap, ops: list[tuple]) -> dict:
    slots: dict[int, int] = {}
    failed = 0
    ignored = 0
    for op in ops:
        kind, ident = op[0], op[1]
        try:
            if kind in ("a", "c", "m"):
                if ident in slots:
                    ignored += 1
                    continue
                if kind == "a":
                    slots[ident] = heap.alloc(op[2])
                elif kind == "c":
                    slots[ident] = heap.calloc(op[2], op[3])
                else:
                    slots[ident] = heap.memalign(op[2], op[3])
            elif kind == "r":
                if op[2] == 0:
                    if ident in slots:
                        heap.free(slots.pop(ident))
                    continue
                slots[ident] = heap.realloc(slots.get(ident, 0) or None, op[2])
            elif kind == "f":
                if ident not in slots:
                    ignored += 1
                    continue
                heap.free(slots.pop(ident))
        except HeapError:
            failed += 1
    return {"commands": len(ops), "failed": failed, "ignored": ignored, "live": slots}


class Session:
    def __init__(self, library: Library, name: str, policy: str, order: str, grow_size: int, mmap_threshold: int, max_bytes: int) -> None:
        self.id = uuid.uuid4().hex[:12]
        self.name = name
        self.order = order
        self.grow_size = grow_size
        self.mmap_threshold = mmap_threshold
        self.max_bytes = max_bytes
        self.heap = Heap(library, POLICIES[policy], ORDERS[order], grow_size, mmap_threshold, max_bytes)
        self.allocations: dict[int, Allocation] = {}
        self.next_handle = 1
        self.seq = 0
        self.lock = threading.RLock()
        self.created_at = time.time()
        self.touched_at = self.created_at

    @property
    def policy(self) -> str:
        return POLICY_NAMES[self.heap.policy]

    def touch(self) -> None:
        self.touched_at = time.time()

    def next_seq(self) -> int:
        self.seq += 1
        return self.seq

    def register(self, address: int, size: int, label: str, kind: str) -> Allocation:
        alloc = Allocation(self.next_handle, address, size, label, kind)
        self.allocations[alloc.handle] = alloc
        self.next_handle += 1
        return alloc

    def by_address(self) -> dict[int, Allocation]:
        return {a.address: a for a in self.allocations.values()}

    def describe(self) -> dict:
        stats = self.heap.stats()
        return {
            "id": self.id,
            "name": self.name,
            "policy": self.policy,
            "order": self.order,
            "grow_size": self.grow_size,
            "mmap_threshold": self.mmap_threshold,
            "max_bytes": self.max_bytes,
            "allocations": len(self.allocations),
            "arena_bytes": stats["arena_bytes"],
            "created_at": self.created_at,
        }

    def snapshot(self, limit: int = 4096) -> dict:
        with self.lock:
            blocks, total = self.heap.blocks(limit)
            owners = self.by_address()
            for block in blocks:
                owner = owners.get(block["address"]) if block["used"] else None
                block["handle"] = owner.handle if owner else None
                block["label"] = owner.label if owner else None
            arenas = self.heap.arenas()
            for block in blocks:
                block["address_hex"] = hex(block["address"])
            free_list = [addr + HEADER for addr in self.heap.free_list(limit)]
            tree = []
            for node in self.heap.tree(1024):
                tree.append({
                    "address": node["address"] + HEADER,
                    "left": node["left"] + HEADER if node["left"] else None,
                    "right": node["right"] + HEADER if node["right"] else None,
                    "size": node["size"],
                    "red": node["red"],
                    "depth": node["depth"],
                })
            return {
                "session": self.describe(),
                "stats": self.heap.stats(),
                "check": self.heap.check(),
                "arenas": arenas,
                "blocks": blocks,
                "total_blocks": total,
                "free_list": free_list,
                "tree": tree,
                "allocations": [
                    {"handle": a.handle, "address": a.address, "address_hex": hex(a.address), "size": a.size, "label": a.label, "kind": a.kind, "usable": self.heap.usable_size(a.address)}
                    for a in sorted(self.allocations.values(), key=lambda x: x.handle)
                ],
            }

    def close(self) -> None:
        with self.lock:
            self.heap.close()
            self.allocations.clear()


class SessionManager:
    def __init__(self, library: Library, store: Store, max_sessions: int = 32, idle_seconds: int = 7200) -> None:
        self.library = library
        self.store = store
        self.max_sessions = max_sessions
        self.idle_seconds = idle_seconds
        self.sessions: dict[str, Session] = {}
        self.lock = threading.Lock()

    def create(self, name: str, policy: str, order: str, grow_size: int, mmap_threshold: int, max_bytes: int) -> Session:
        self.expire()
        with self.lock:
            if len(self.sessions) >= self.max_sessions:
                raise OverflowError(f"limite de {self.max_sessions} sessões abertas atingido")
            session = Session(self.library, name, policy, order, grow_size, mmap_threshold, max_bytes)
            self.sessions[session.id] = session
        self.store.create_session(session.id, name, policy, order, grow_size, mmap_threshold, max_bytes)
        return session

    def get(self, sid: str) -> Session | None:
        session = self.sessions.get(sid)
        if session:
            session.touch()
        return session

    def remove(self, sid: str) -> bool:
        with self.lock:
            session = self.sessions.pop(sid, None)
        if not session:
            return False
        session.close()
        self.store.close_session(sid)
        return True

    def expire(self) -> None:
        now = time.time()
        stale = [sid for sid, s in list(self.sessions.items()) if now - s.touched_at > self.idle_seconds]
        for sid in stale:
            self.remove(sid)

    def close_all(self) -> None:
        for sid in list(self.sessions):
            self.remove(sid)
