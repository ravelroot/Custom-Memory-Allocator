export type Policy = "first-fit" | "next-fit" | "best-fit";

export interface Stats {
  arena_count: number;
  arena_bytes: number;
  used_blocks: number;
  free_blocks: number;
  used_bytes: number;
  free_bytes: number;
  requested_bytes: number;
  largest_free: number;
  large_blocks: number;
  large_bytes: number;
  peak_arena_bytes: number;
  total_allocs: number;
  total_frees: number;
  total_reallocs: number;
  failed_allocs: number;
  splits: number;
  coalesces: number;
  os_requests: number;
  os_releases: number;
  search_steps: number;
  fragmentation: number;
  utilization: number;
}

export interface Arena {
  index: number;
  base: number;
  length: number;
  first: number;
  end: number;
  source: string;
  large: boolean;
}

export interface Block {
  arena: number;
  offset: number;
  size: number;
  payload: number;
  request: number;
  used: boolean;
  large: boolean;
  address: number;
  address_hex: string;
  handle: number | null;
  label: string | null;
}

export interface TreeNode {
  address: number;
  left: number | null;
  right: number | null;
  size: number;
  red: boolean;
  depth: number;
}

export interface Allocation {
  handle: number;
  address: number;
  address_hex: string;
  size: number;
  usable: number;
  label: string;
  kind: string;
}

export interface SessionInfo {
  id: string;
  name: string;
  policy: Policy;
  order: string;
  grow_size: number;
  mmap_threshold: number;
  max_bytes: number;
  allocations: number;
  arena_bytes: number;
  created_at: number;
}

export interface Snapshot {
  session: SessionInfo;
  stats: Stats;
  check: { ok: boolean; message: string };
  arenas: Arena[];
  blocks: Block[];
  total_blocks: number;
  free_list: number[];
  tree: TreeNode[];
  allocations: Allocation[];
}

export interface Operation {
  seq: number;
  kind: string;
  handle: number | null;
  size: number | null;
  offset: number | null;
  ok: number;
  error: string | null;
  elapsed_ns: number;
  created_at: number;
}

export interface BenchResult {
  allocator: string;
  ops_per_second: number;
  seconds: number;
  peak_arena_bytes: number;
  fragmentation: number;
  utilization: number;
  search_steps: number;
  failed: number;
}

export interface BenchRun {
  id: number;
  operations: number;
  seed: number;
  min_size: number;
  max_size: number;
  created_at: number;
  results: BenchResult[];
}

export interface TraceRow {
  id: number;
  name: string;
  commands: number;
  content?: string;
  created_at: number;
  updated_at: number;
}

export interface ComparePolicy {
  policy: Policy;
  seconds: number;
  result: { commands: number; failed: number; ignored: number };
  stats: Stats;
  check: { ok: boolean; message: string };
  arenas: Arena[];
  blocks: Block[];
  total_blocks: number;
}

export class ApiError extends Error {
  status: number;
  constructor(status: number, message: string) {
    super(message);
    this.status = status;
  }
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  const res = await fetch(path, {
    method,
    headers: body === undefined ? undefined : { "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  if (res.status === 204) return undefined as T;
  const text = await res.text();
  const data = text ? JSON.parse(text) : null;
  if (!res.ok) {
    let message = `erro ${res.status}`;
    if (data && typeof data.detail === "string") message = data.detail;
    else if (data && Array.isArray(data.detail)) message = data.detail.map((d: { msg: string }) => d.msg).join("; ");
    throw new ApiError(res.status, message);
  }
  return data as T;
}

export const api = {
  info: () => request<Record<string, unknown>>("GET", "/api/info"),
  sessions: () => request<SessionInfo[]>("GET", "/api/sessions"),
  createSession: (body: Partial<SessionInfo> & { name: string }) => request<SessionInfo>("POST", "/api/sessions", body),
  deleteSession: (id: string) => request<void>("DELETE", `/api/sessions/${id}`),
  snapshot: (id: string) => request<Snapshot>("GET", `/api/sessions/${id}/heap`),
  operations: (id: string) => request<Operation[]>("GET", `/api/sessions/${id}/operations?limit=60`),
  alloc: (id: string, size: number, label: string) => request<Allocation>("POST", `/api/sessions/${id}/alloc`, { size, label }),
  calloc: (id: string, count: number, size: number, label: string) => request<Allocation>("POST", `/api/sessions/${id}/calloc`, { count, size, label }),
  memalign: (id: string, alignment: number, size: number, label: string) => request<Allocation>("POST", `/api/sessions/${id}/memalign`, { alignment, size, label }),
  realloc: (id: string, handle: number, size: number) => request<Allocation>("POST", `/api/sessions/${id}/realloc`, { handle, size }),
  free: (id: string, handle: number) => request<unknown>("POST", `/api/sessions/${id}/free`, { handle }),
  freeAll: (id: string) => request<unknown>("POST", `/api/sessions/${id}/free-all`),
  write: (id: string, handle: number, text: string) => request<unknown>("POST", `/api/sessions/${id}/write`, { handle, text }),
  read: (id: string, handle: number) => request<{ hex: string; text: string; length: number; usable: number }>("GET", `/api/sessions/${id}/read/${handle}?length=128`),
  policy: (id: string, policy: Policy) => request<SessionInfo>("PUT", `/api/sessions/${id}/policy`, { policy }),
  trim: (id: string) => request<{ released: number }>("POST", `/api/sessions/${id}/trim`),
  reset: (id: string) => request<SessionInfo>("POST", `/api/sessions/${id}/reset`),
  runTrace: (id: string, content: string) => request<{ commands: number; failed: number; ignored: number }>("POST", `/api/sessions/${id}/trace`, { content }),
  traces: () => request<TraceRow[]>("GET", "/api/traces"),
  trace: (tid: number) => request<TraceRow>("GET", `/api/traces/${tid}`),
  saveTrace: (name: string, content: string) => request<TraceRow>("POST", "/api/traces", { name, content }),
  deleteTrace: (tid: number) => request<void>("DELETE", `/api/traces/${tid}`),
  generate: (operations: number, seed: number, min_size: number, max_size: number) =>
    request<{ content: string; commands: number }>("POST", "/api/traces/generate", { operations, seed, min_size, max_size }),
  compare: (content: string) => request<{ commands: number; policies: ComparePolicy[] }>("POST", "/api/compare", { content }),
  bench: (operations: number, seed: number, min_size: number, max_size: number) =>
    request<BenchRun>("POST", "/api/benchmarks", { operations, seed, min_size, max_size }),
  benches: () => request<BenchRun[]>("GET", "/api/benchmarks?limit=10"),
};
