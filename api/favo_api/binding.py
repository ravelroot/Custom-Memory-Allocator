from __future__ import annotations

import ctypes
import os
import sys
from ctypes import POINTER, Structure, c_char_p, c_double, c_int, c_size_t, c_uint64, c_void_p
from pathlib import Path

c_uintptr = ctypes.c_size_t

POLICIES = {"first-fit": 0, "next-fit": 1, "best-fit": 2}
POLICY_NAMES = {value: key for key, value in POLICIES.items()}
ORDERS = {"address": 0, "lifo": 1}
STATUS = {
    0: "ok",
    1: "memória insuficiente",
    2: "ponteiro ou argumento inválido",
    3: "double free detectado",
    4: "ponteiro não pertence a este heap",
    5: "heap corrompido",
}


class Config(Structure):
    _fields_ = [
        ("policy", c_int),
        ("source", c_int),
        ("order", c_int),
        ("thread_safe", c_int),
        ("abort_on_error", c_int),
        ("release_empty", c_int),
        ("grow_size", c_size_t),
        ("mmap_threshold", c_size_t),
        ("max_bytes", c_size_t),
    ]


class Stats(Structure):
    _fields_ = [
        ("arena_count", c_size_t),
        ("arena_bytes", c_size_t),
        ("used_blocks", c_size_t),
        ("free_blocks", c_size_t),
        ("used_bytes", c_size_t),
        ("free_bytes", c_size_t),
        ("requested_bytes", c_size_t),
        ("largest_free", c_size_t),
        ("large_blocks", c_size_t),
        ("large_bytes", c_size_t),
        ("peak_arena_bytes", c_size_t),
        ("total_allocs", c_uint64),
        ("total_frees", c_uint64),
        ("total_reallocs", c_uint64),
        ("failed_allocs", c_uint64),
        ("splits", c_uint64),
        ("coalesces", c_uint64),
        ("os_requests", c_uint64),
        ("os_releases", c_uint64),
        ("search_steps", c_uint64),
        ("fragmentation", c_double),
        ("utilization", c_double),
    ]


class BlockInfo(Structure):
    _fields_ = [
        ("arena", c_size_t),
        ("offset", c_size_t),
        ("size", c_size_t),
        ("payload", c_size_t),
        ("request", c_size_t),
        ("used", c_int),
        ("large", c_int),
        ("address", c_uintptr),
    ]


class ArenaInfo(Structure):
    _fields_ = [
        ("index", c_size_t),
        ("base", c_uintptr),
        ("length", c_size_t),
        ("first_offset", c_size_t),
        ("end_offset", c_size_t),
        ("source", c_int),
        ("large", c_int),
    ]


class TreeNode(Structure):
    _fields_ = [
        ("address", c_uintptr),
        ("left", c_uintptr),
        ("right", c_uintptr),
        ("size", c_size_t),
        ("red", c_int),
        ("depth", c_int),
    ]


class BenchResult(Structure):
    _fields_ = [
        ("policy", c_int),
        ("operations", c_uint64),
        ("seconds", c_double),
        ("ops_per_second", c_double),
        ("peak_arena_bytes", c_size_t),
        ("fragmentation", c_double),
        ("utilization", c_double),
        ("search_steps", c_uint64),
        ("failed", c_uint64),
    ]


def _candidates() -> list[Path]:
    names = ["favo.dll", "libfavo.dll"] if sys.platform == "win32" else ["libfavo.dylib"] if sys.platform == "darwin" else ["libfavo.so"]
    here = Path(__file__).resolve().parent
    root = here.parent.parent
    folders = [here, root / "build", root / "build" / "Release", root / "build" / "Debug", Path("/usr/local/lib"), Path("/usr/lib")]
    return [folder / name for folder in folders for name in names]


def find_library() -> Path:
    configured = os.environ.get("FAVO_LIBRARY")
    if configured:
        path = Path(configured)
        if path.is_file():
            return path
        raise FileNotFoundError(f"FAVO_LIBRARY aponta para um arquivo inexistente: {configured}")
    for path in _candidates():
        if path.is_file():
            return path
    raise FileNotFoundError("biblioteca do Favo não encontrada: compile com 'cmake --build build' ou defina FAVO_LIBRARY")


class Library:
    def __init__(self, path: Path | None = None) -> None:
        self.path = path or find_library()
        lib = ctypes.CDLL(str(self.path))
        self.lib = lib
        heap = c_void_p
        signatures = {
            "favo_config_default": (None, [POINTER(Config)]),
            "favo_heap_create": (heap, [POINTER(Config)]),
            "favo_heap_destroy": (None, [heap]),
            "favo_heap_reset": (None, [heap]),
            "favo_heap_alloc": (c_void_p, [heap, c_size_t]),
            "favo_heap_calloc": (c_void_p, [heap, c_size_t, c_size_t]),
            "favo_heap_realloc": (c_void_p, [heap, c_void_p, c_size_t]),
            "favo_heap_memalign": (c_void_p, [heap, c_size_t, c_size_t]),
            "favo_heap_free": (c_int, [heap, c_void_p]),
            "favo_heap_usable_size": (c_size_t, [heap, c_void_p]),
            "favo_heap_set_policy": (c_int, [heap, c_int]),
            "favo_heap_get_policy": (c_int, [heap]),
            "favo_heap_trim": (c_size_t, [heap, c_size_t]),
            "favo_heap_stats": (None, [heap, POINTER(Stats)]),
            "favo_heap_check": (c_int, [heap, c_char_p, c_size_t]),
            "favo_heap_snapshot": (c_size_t, [heap, POINTER(BlockInfo), c_size_t]),
            "favo_heap_arenas": (c_size_t, [heap, POINTER(ArenaInfo), c_size_t]),
            "favo_heap_free_list": (c_size_t, [heap, POINTER(c_uintptr), c_size_t]),
            "favo_heap_tree": (c_size_t, [heap, POINTER(TreeNode), c_size_t]),
            "favo_heap_last_error": (c_int, [heap]),
            "favo_bench_run": (c_int, [c_int, c_uint64, c_uint64, c_size_t, c_size_t, POINTER(BenchResult)]),
            "favo_bench_system": (c_int, [c_uint64, c_uint64, c_size_t, c_size_t, POINTER(BenchResult)]),
            "favo_version": (c_char_p, []),
            "favo_page_size": (c_size_t, []),
        }
        for name, (restype, argtypes) in signatures.items():
            fn = getattr(lib, name)
            fn.restype = restype
            fn.argtypes = argtypes

    @property
    def version(self) -> str:
        return self.lib.favo_version().decode()

    @property
    def page_size(self) -> int:
        return int(self.lib.favo_page_size())

    def bench(self, policy: int | None, operations: int, seed: int, min_size: int, max_size: int) -> dict:
        result = BenchResult()
        if policy is None:
            rc = self.lib.favo_bench_system(operations, seed, min_size, max_size, ctypes.byref(result))
        else:
            rc = self.lib.favo_bench_run(policy, operations, seed, min_size, max_size, ctypes.byref(result))
        if rc:
            raise ValueError(STATUS.get(rc, "erro"))
        return {
            "allocator": "malloc do sistema" if policy is None else POLICY_NAMES[policy],
            "operations": int(result.operations),
            "seconds": result.seconds,
            "ops_per_second": result.ops_per_second,
            "peak_arena_bytes": int(result.peak_arena_bytes),
            "fragmentation": result.fragmentation,
            "utilization": result.utilization,
            "search_steps": int(result.search_steps),
            "failed": int(result.failed),
        }


class HeapError(Exception):
    def __init__(self, code: int) -> None:
        super().__init__(STATUS.get(code, "erro desconhecido"))
        self.code = code


class Heap:
    def __init__(self, library: Library, policy: int = 0, order: int = 0, grow_size: int = 64 * 1024,
                 mmap_threshold: int = 32 * 1024, max_bytes: int = 16 * 1024 * 1024) -> None:
        self.library = library
        cfg = Config()
        library.lib.favo_config_default(ctypes.byref(cfg))
        cfg.policy = policy
        cfg.order = order
        cfg.source = 2
        cfg.thread_safe = 1
        cfg.abort_on_error = 0
        cfg.grow_size = grow_size
        cfg.mmap_threshold = mmap_threshold
        cfg.max_bytes = max_bytes
        self.config = cfg
        self.handle = library.lib.favo_heap_create(ctypes.byref(cfg))
        if not self.handle:
            raise MemoryError("não foi possível criar o heap")

    def close(self) -> None:
        if self.handle:
            self.library.lib.favo_heap_destroy(self.handle)
            self.handle = None

    def _error(self) -> HeapError:
        return HeapError(self.library.lib.favo_heap_last_error(self.handle))

    def alloc(self, size: int) -> int:
        ptr = self.library.lib.favo_heap_alloc(self.handle, size)
        if not ptr:
            raise self._error()
        return ptr

    def calloc(self, count: int, size: int) -> int:
        ptr = self.library.lib.favo_heap_calloc(self.handle, count, size)
        if not ptr:
            raise self._error()
        return ptr

    def realloc(self, ptr: int, size: int) -> int:
        out = self.library.lib.favo_heap_realloc(self.handle, ptr, size)
        if not out:
            raise self._error()
        return out

    def memalign(self, alignment: int, size: int) -> int:
        ptr = self.library.lib.favo_heap_memalign(self.handle, alignment, size)
        if not ptr:
            raise self._error()
        return ptr

    def free(self, ptr: int) -> None:
        rc = self.library.lib.favo_heap_free(self.handle, ptr)
        if rc:
            raise HeapError(rc)

    def usable_size(self, ptr: int) -> int:
        return int(self.library.lib.favo_heap_usable_size(self.handle, ptr))

    def set_policy(self, policy: int) -> None:
        rc = self.library.lib.favo_heap_set_policy(self.handle, policy)
        if rc:
            raise HeapError(rc)

    @property
    def policy(self) -> int:
        return int(self.library.lib.favo_heap_get_policy(self.handle))

    def trim(self) -> int:
        return int(self.library.lib.favo_heap_trim(self.handle, 0))

    def reset(self) -> None:
        self.library.lib.favo_heap_reset(self.handle)

    def write(self, ptr: int, data: bytes) -> None:
        ctypes.memmove(ptr, data, len(data))

    def read(self, ptr: int, length: int) -> bytes:
        return ctypes.string_at(ptr, length)

    def stats(self) -> dict:
        s = Stats()
        self.library.lib.favo_heap_stats(self.handle, ctypes.byref(s))
        return {name: getattr(s, name) for name, _ in Stats._fields_}

    def check(self) -> dict:
        buf = ctypes.create_string_buffer(256)
        rc = self.library.lib.favo_heap_check(self.handle, buf, len(buf))
        return {"ok": rc == 0, "message": buf.value.decode(errors="replace") if rc else "heap consistente"}

    def arenas(self) -> list[dict]:
        n = self.library.lib.favo_heap_arenas(self.handle, None, 0)
        arr = (ArenaInfo * max(n, 1))()
        n = self.library.lib.favo_heap_arenas(self.handle, arr, n)
        out = []
        for a in arr[:n]:
            out.append({
                "index": int(a.index),
                "base": int(a.base),
                "length": int(a.length),
                "first": int(a.first_offset),
                "end": int(a.end_offset),
                "source": "mmap" if a.source == 2 else "sbrk",
                "large": bool(a.large),
            })
        return out

    def blocks(self, limit: int = 4096) -> tuple[list[dict], int]:
        total = self.library.lib.favo_heap_snapshot(self.handle, None, 0)
        count = min(total, limit)
        arr = (BlockInfo * max(count, 1))()
        self.library.lib.favo_heap_snapshot(self.handle, arr, count)
        out = []
        for b in arr[:count]:
            out.append({
                "arena": int(b.arena),
                "offset": int(b.offset),
                "size": int(b.size),
                "payload": int(b.payload),
                "request": int(b.request),
                "used": bool(b.used),
                "large": bool(b.large),
                "address": int(b.address),
            })
        return out, int(total)

    def free_list(self, limit: int = 4096) -> list[int]:
        total = self.library.lib.favo_heap_free_list(self.handle, None, 0)
        count = min(total, limit)
        arr = (c_uintptr * max(count, 1))()
        self.library.lib.favo_heap_free_list(self.handle, arr, count)
        return [int(x) for x in arr[:count]]

    def tree(self, limit: int = 1024) -> list[dict]:
        total = self.library.lib.favo_heap_tree(self.handle, None, 0)
        count = min(total, limit)
        arr = (TreeNode * max(count, 1))()
        self.library.lib.favo_heap_tree(self.handle, arr, count)
        return [
            {"address": int(n.address), "left": int(n.left), "right": int(n.right), "size": int(n.size), "red": bool(n.red), "depth": int(n.depth)}
            for n in arr[:count]
        ]
