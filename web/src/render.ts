import type { Arena, Block, TreeNode } from "./api";

export function esc(value: unknown): string {
  return String(value ?? "")
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

export function bytes(n: number): string {
  if (n >= 1 << 20) return `${(n / (1 << 20)).toFixed(1).replace(".", ",")} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1).replace(".", ",")} KiB`;
  return `${n} B`;
}

export function pct(v: number): string {
  return `${(v * 100).toFixed(1).replace(".", ",")}%`;
}

export function num(v: number): string {
  return Math.round(v).toLocaleString("pt-BR");
}

export function hex(n: number): string {
  return `0x${n.toString(16)}`;
}

export function heapMap(arenas: Arena[], blocks: Block[], selected: number | null, compact = false): string {
  if (!arenas.length) return `<p class="empty">Nenhuma arena ainda. A primeira alocação pede memória ao sistema com mmap.</p>`;
  const byArena = new Map<number, Block[]>();
  for (const b of blocks) {
    const list = byArena.get(b.arena) ?? [];
    list.push(b);
    byArena.set(b.arena, list);
  }
  const rows = arenas.map((a) => {
    const list = byArena.get(a.index) ?? [];
    const cells = list
      .map((b) => {
        const cls = b.large ? "large" : b.used ? "used" : "free";
        const sel = b.used && b.address === selected ? " selected" : "";
        const label = b.used ? (b.label ? b.label : b.handle ? `#${b.handle}` : "") : "";
        const tip = `${b.used ? (b.large ? "bloco grande" : "ocupado") : "livre"} · bloco ${bytes(b.size)} · útil ${bytes(b.payload)}${b.used ? ` · pedido ${bytes(b.request)}` : ""} · ${b.address_hex}${b.handle ? ` · #${b.handle}` : ""}`;
        const text = !compact && label ? `<span>${esc(label)}</span>` : "";
        if (!b.used) return `<div class="blk ${cls}" style="flex-grow:${b.size}" title="${esc(tip)}"></div>`;
        return `<button type="button" class="blk ${cls}${sel}" style="flex-grow:${b.size}" data-address="${b.address}" title="${esc(tip)}" aria-label="${esc(tip)}">${text}</button>`;
      })
      .join("");
    return `<div class="arena">
      <div class="arena-head"><span>arena ${a.index} · ${a.large ? "bloco grande (mmap próprio)" : a.source} · ${bytes(a.length)}</span><span>${hex(a.base)}</span></div>
      <div class="bar">${cells}</div>
    </div>`;
  });
  return `<div class="map">${rows.join("")}</div>`;
}

export function legend(): string {
  return `<div class="legend"><span><i class="u"></i>ocupado</span><span><i class="f"></i>livre</span><span><i class="l"></i>bloco grande (mmap)</span></div>`;
}

export function treeSvg(nodes: TreeNode[]): string {
  if (!nodes.length) return `<p class="empty">Árvore vazia: não há blocos livres.</p>`;
  const map = new Map<number, TreeNode>();
  nodes.forEach((n) => map.set(n.address, n));
  const root = nodes.find((n) => n.depth === 0);
  if (!root) return "";
  const pos = new Map<number, { x: number; y: number }>();
  let order = 0;
  const walk = (n: TreeNode | undefined, depth: number) => {
    if (!n) return;
    walk(n.left !== null ? map.get(n.left) : undefined, depth + 1);
    pos.set(n.address, { x: order++, y: depth });
    walk(n.right !== null ? map.get(n.right) : undefined, depth + 1);
  };
  walk(root, 0);
  const sx = 64;
  const sy = 64;
  const maxDepth = Math.max(...[...pos.values()].map((p) => p.y));
  const width = Math.max(order * sx + 20, 200);
  const height = (maxDepth + 1) * sy + 20;
  const px = (a: number) => (pos.get(a)!.x * sx + 10 + sx / 2);
  const py = (a: number) => (pos.get(a)!.y * sy + 30);
  let edges = "";
  let circles = "";
  for (const n of nodes) {
    if (!pos.has(n.address)) continue;
    for (const c of [n.left, n.right]) {
      if (c !== null && pos.has(c)) edges += `<line x1="${px(n.address)}" y1="${py(n.address)}" x2="${px(c)}" y2="${py(c)}" stroke="currentColor" stroke-width="1.5" opacity="0.6"/>`;
    }
    const fill = n.red ? "#AB9D9D" : "currentColor";
    const textFill = n.red ? "#000" : "var(--bg)";
    circles += `<g><title>${n.red ? "vermelho" : "negro"} · ${bytes(n.size)} · ${hex(n.address)}</title><circle cx="${px(n.address)}" cy="${py(n.address)}" r="22" fill="${fill}" stroke="currentColor" stroke-width="1.5"/><text x="${px(n.address)}" y="${py(n.address) + 4}" text-anchor="middle" fill="${textFill}">${esc(bytes(n.size).replace(" ", ""))}</text></g>`;
  }
  return `<svg width="${width}" height="${height}" viewBox="0 0 ${width} ${height}" role="img" aria-label="Árvore rubro-negra dos blocos livres">${edges}${circles}</svg>`;
}

export const logoSvg = `<svg viewBox="0 0 120 120" aria-hidden="true"><rect x="20" y="94" width="80" height="14" fill="currentColor"/><rect x="20" y="76" width="80" height="14" fill="currentColor"/><rect x="22" y="60" width="76" height="10" fill="none" stroke="currentColor" stroke-width="4"/><line x1="12" y1="46" x2="108" y2="46" stroke="currentColor" stroke-width="4" stroke-dasharray="8 6"/><line x1="60" y1="38" x2="60" y2="20" stroke="currentColor" stroke-width="4"/><polygon points="50,22 60,8 70,22" fill="currentColor"/></svg>`;
