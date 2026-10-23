import "./style.css";
import { api, ApiError, type BenchRun, type ComparePolicy, type Operation, type Policy, type SessionInfo, type Snapshot, type TraceRow } from "./api";
import { bytes, esc, heapMap, hex, legend, logoSvg, num, pct, treeSvg } from "./render";

type View = "heap" | "compare" | "bench" | "traces";

const state = {
  view: "heap" as View,
  sessions: [] as SessionInfo[],
  sessionId: null as string | null,
  snap: null as Snapshot | null,
  ops: [] as Operation[],
  selected: null as number | null,
  dump: null as { hex: string; text: string } | null,
  socket: null as WebSocket | null,
  live: false,
  compare: null as { commands: number; policies: ComparePolicy[] } | null,
  compareText: "",
  benches: [] as BenchRun[],
  traces: [] as TraceRow[],
  traceEdit: { name: "", content: "" },
  busy: false,
};

const app = document.querySelector<HTMLDivElement>("#app")!;
const POLICIES: Policy[] = ["first-fit", "next-fit", "best-fit"];
const SAMPLE = "a 1 1200\na 2 300\na 3 2000\na 4 120\na 5 900\nf 2\nf 4\na 6 100\nr 1 2400\nm 7 256 500\nc 8 32 16\nf 3\nf 5\n";

function toast(message: string, error = false) {
  const el = document.createElement("div");
  el.className = `toast${error ? " err" : ""}`;
  el.setAttribute("role", error ? "alert" : "status");
  el.textContent = message;
  document.body.appendChild(el);
  setTimeout(() => el.remove(), error ? 5000 : 2500);
}

async function guard<T>(fn: () => Promise<T>): Promise<T | undefined> {
  try {
    state.busy = true;
    return await fn();
  } catch (err) {
    toast(err instanceof ApiError || err instanceof Error ? err.message : String(err), true);
    return undefined;
  } finally {
    state.busy = false;
  }
}

function themeToggle(): string {
  return `<button class="icon-btn" type="button" data-action="theme" aria-label="Alternar tema claro e escuro"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><circle cx="12" cy="12" r="5"/><path d="M12 1v3M12 20v3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M1 12h3M20 12h3M4.2 19.8l2.1-2.1M17.7 6.3l2.1-2.1"/></svg>Tema</button>`;
}

function shell(): string {
  const tab = (v: View, label: string) => `<button type="button" data-view="${v}" ${state.view === v ? 'aria-current="page"' : ""}>${label}</button>`;
  return `<header class="topbar">
    <a class="brand" href="/">${logoSvg}<span>Favo <small>alocador de memória</small></span></a>
    <nav class="tabs" aria-label="Seções">${tab("heap", "Heap")}${tab("compare", "Comparar")}${tab("bench", "Desempenho")}${tab("traces", "Traces")}</nav>
    <span class="spacer"></span>
    <a class="icon-btn" href="/docs" target="_blank" rel="noopener">API</a>
    ${themeToggle()}
  </header>
  <main id="view"></main>`;
}

function render() {
  app.innerHTML = shell();
  renderView();
}

function renderView() {
  const view = document.querySelector<HTMLElement>("#view")!;
  if (state.view === "heap") view.innerHTML = heapView();
  else if (state.view === "compare") view.innerHTML = compareView();
  else if (state.view === "bench") view.innerHTML = benchView();
  else view.innerHTML = tracesView();
}

function sessionCard(): string {
  const options = state.sessions.map((s) => `<option value="${s.id}" ${s.id === state.sessionId ? "selected" : ""}>${esc(s.name)} · ${s.policy}</option>`).join("");
  return `<section class="card">
    <h2>Sessão</h2>
    ${state.sessions.length ? `<label>Heap ativo<select data-change="session">${options}</select></label>` : `<p class="hint">Cada sessão é um heap isolado criado pela biblioteca em C.</p>`}
    <form data-form="session" style="margin-top:12px; display:flex; flex-direction:column; gap:8px">
      <div class="row">
        <label>Nome<input name="name" value="Heap ${state.sessions.length + 1}" maxlength="80" required></label>
        <label>Ordem da lista<select name="order"><option value="address">endereço</option><option value="lifo">LIFO</option></select></label>
      </div>
      <div class="row">
        <label>Crescimento<select name="grow_size"><option value="16384">16 KiB</option><option value="65536" selected>64 KiB</option><option value="262144">256 KiB</option><option value="1048576">1 MiB</option></select></label>
        <label>Limite mmap<select name="mmap_threshold"><option value="0">desligado</option><option value="32768" selected>32 KiB</option><option value="131072">128 KiB</option></select></label>
      </div>
      <button class="btn" type="submit">Nova sessão</button>
    </form>
    ${state.sessionId ? `<p class="hint"><span class="status-dot ${state.live ? "on" : ""}"></span>${state.live ? "atualização ao vivo via WebSocket" : "conectando..."}</p>` : ""}
  </section>`;
}

function controlsCard(): string {
  if (!state.snap) return "";
  const policy = state.snap.session.policy;
  const seg = POLICIES.map((p) => `<button type="button" data-policy="${p}" aria-pressed="${p === policy}">${p.replace("-fit", "")}</button>`).join("");
  const quick = [16, 64, 200, 1024, 4096, 16384, 65536].map((n) => `<button type="button" data-quick="${n}">${bytes(n)}</button>`).join("");
  return `<section class="card">
    <h2>Algoritmo</h2>
    <div class="segmented" role="group" aria-label="Política de alocação">${seg}</div>
    <p class="hint">${policy === "best-fit" ? "Busca na árvore rubro-negra o menor bloco livre que serve." : policy === "next-fit" ? "Continua a busca na lista circular de onde parou na última vez." : "Percorre a lista circular desde o início e usa o primeiro bloco que serve."}</p>
  </section>
  <section class="card">
    <h2>Alocar</h2>
    <form data-form="alloc" class="row">
      <label>Bytes<input name="size" type="number" min="0" max="8388608" value="256" required></label>
      <label>Rótulo<input name="label" maxlength="40" placeholder="opcional"></label>
      <button class="btn" type="submit">malloc</button>
    </form>
    <div class="chips" aria-label="Tamanhos rápidos">${quick}</div>
    <h3>calloc e memalign</h3>
    <form data-form="calloc" class="row">
      <label>Qtd<input name="count" type="number" min="0" value="16" required></label>
      <label>Bytes<input name="size" type="number" min="0" value="32" required></label>
      <button class="btn ghost" type="submit">calloc</button>
    </form>
    <form data-form="memalign" class="row" style="margin-top:8px">
      <label>Alinhamento<select name="alignment"><option>32</option><option>64</option><option>256</option><option selected>4096</option></select></label>
      <label>Bytes<input name="size" type="number" min="0" value="100" required></label>
      <button class="btn ghost" type="submit">memalign</button>
    </form>
    <h3>Heap inteiro</h3>
    <div class="row">
      <button class="btn ghost" type="button" data-action="free-all">Liberar tudo</button>
      <button class="btn ghost" type="button" data-action="trim">Devolver ao sistema</button>
      <button class="btn danger" type="button" data-action="reset">Zerar</button>
    </div>
  </section>
  <section class="card">
    <h2>Executar trace</h2>
    <form data-form="trace" style="display:flex; flex-direction:column; gap:8px">
      <textarea name="content" aria-label="Comandos do trace" spellcheck="false">${esc(SAMPLE)}</textarea>
      <button class="btn ghost" type="submit">Executar nesta sessão</button>
    </form>
    <p class="hint">a id bytes · c id qtd bytes · m id alinhamento bytes · r id bytes · f id</p>
  </section>`;
}

function statsRow(): string {
  const s = state.snap!.stats;
  const card = (label: string, value: string, extra = "") => `<div class="stat"><span>${label}</span><strong>${value}</strong>${extra ? `<em>${extra}</em>` : ""}</div>`;
  return `<div class="stats">
    ${card("Memória do sistema", bytes(s.arena_bytes), `${s.arena_count} arena(s) · pico ${bytes(s.peak_arena_bytes)}`)}
    ${card("Ocupado", bytes(s.used_bytes), `${s.used_blocks} bloco(s) · pedido ${bytes(s.requested_bytes)}`)}
    ${card("Livre", bytes(s.free_bytes), `${s.free_blocks} bloco(s) · maior ${bytes(s.largest_free)}`)}
    ${card("Fragmentação", pct(s.fragmentation), "1 − maior livre ÷ total livre")}
    ${card("Aproveitamento", pct(s.utilization), "pedido ÷ memória do sistema")}
    ${card("Busca", num(s.search_steps), `${num(s.splits)} divisões · ${num(s.coalesces)} fusões`)}
  </div>`;
}

function detailCard(): string {
  const snap = state.snap!;
  const a = snap.allocations.find((x) => x.address === state.selected);
  if (!a) return `<section class="card detail"><h2>Bloco selecionado</h2><p class="empty">Clique num bloco ocupado do mapa ou da tabela para ver os detalhes, gravar dados, realocar ou liberar.</p></section>`;
  const block = snap.blocks.find((b) => b.address === a.address);
  const align = a.address & -a.address;
  return `<section class="card detail">
    <h2>Bloco #${a.handle}${a.label ? ` · ${esc(a.label)}` : ""}</h2>
    <dl>
      <dt>Endereço</dt><dd>${a.address_hex} (alinhado a ${align >= 4096 ? bytes(align) : `${align} B`})</dd>
      <dt>Pedido</dt><dd>${bytes(a.size)} via ${esc(a.kind)}</dd>
      <dt>Útil</dt><dd>${bytes(a.usable)}</dd>
      <dt>Bloco inteiro</dt><dd>${block ? `${bytes(block.size)} (cabeçalho 16 B + rodapé 8 B)` : "-"}</dd>
      <dt>Arena</dt><dd>${block ? `${block.arena}, deslocamento ${block.offset}` : "-"}</dd>
    </dl>
    <form data-form="realloc" class="row">
      <label>Novo tamanho<input name="size" type="number" min="1" value="${a.size * 2}" required></label>
      <button class="btn ghost" type="submit">realloc</button>
      <button class="btn danger" type="button" data-free="${a.handle}">free</button>
    </form>
    <form data-form="write" class="row" style="margin-top:8px">
      <label>Gravar texto no bloco<input name="text" maxlength="2000" placeholder="ex.: olá, heap"></label>
      <button class="btn ghost" type="submit">Gravar</button>
      <button class="btn ghost" type="button" data-action="read">Ler</button>
    </form>
    ${state.dump ? `<pre class="hexdump">${esc(state.dump.hex)}\n\ntexto: ${esc(state.dump.text)}</pre>` : ""}
  </section>`;
}

function freeListCard(): string {
  const snap = state.snap!;
  const items = snap.free_list.map((addr) => {
    const b = snap.blocks.find((x) => x.address === addr);
    return `<span class="node">${hex(addr).slice(-6)} · ${b ? bytes(b.size) : "?"}</span>`;
  });
  const body = items.length ? `${items.join('<span class="arrow">→</span>')}<span class="arrow">↺</span>` : `<p class="empty">Lista vazia.</p>`;
  return `<section class="card"><h2>Lista circular de livres</h2><div class="freelist">${body}</div><p class="hint">Ordem ${snap.session.order === "lifo" ? "LIFO" : "por endereço"}. First-Fit e Next-Fit percorrem esta lista.</p></section>`;
}

function treeCard(): string {
  return `<section class="card"><h2>Árvore rubro-negra</h2><div class="tree-wrap">${treeSvg(state.snap!.tree)}</div><p class="hint">Chave: tamanho e endereço. Best-Fit desce por ela em O(log n). Nós rosados são vermelhos.</p></section>`;
}

function allocTable(): string {
  const rows = state.snap!.allocations
    .map((a) => `<tr class="clickable" data-select="${a.address}" tabindex="0"><td>#${a.handle}</td><td>${esc(a.label || "-")}</td><td class="mono">${a.address_hex}</td><td class="num">${num(a.size)}</td><td class="num">${num(a.usable)}</td><td>${esc(a.kind)}</td></tr>`)
    .join("");
  return `<section class="card"><h2>Blocos alocados</h2>${rows ? `<div class="table-wrap"><table><thead><tr><th>#</th><th>Rótulo</th><th>Endereço</th><th class="num">Pedido</th><th class="num">Útil</th><th>Origem</th></tr></thead><tbody>${rows}</tbody></table></div>` : `<p class="empty">Nada alocado.</p>`}</section>`;
}

function opsTable(): string {
  const rows = state.ops
    .slice(0, 30)
    .map((o) => `<tr><td class="num">${o.seq}</td><td>${o.kind}</td><td>${o.handle ? `#${o.handle}` : "-"}</td><td class="num">${o.size ?? "-"}</td><td class="num">${o.offset ?? "-"}</td><td class="num">${num(o.elapsed_ns / 1000)} µs</td><td>${o.ok ? '<span class="badge ok">ok</span>' : `<span class="badge err">${esc(o.error)}</span>`}</td></tr>`)
    .join("");
  return `<section class="card"><h2>Histórico (SQLite)</h2>${rows ? `<div class="table-wrap"><table><thead><tr><th class="num">Seq</th><th>Operação</th><th>Bloco</th><th class="num">Bytes</th><th class="num">Desloc.</th><th class="num">Tempo</th><th>Status</th></tr></thead><tbody>${rows}</tbody></table></div>` : `<p class="empty">Sem operações ainda.</p>`}</section>`;
}

function heapView(): string {
  if (!state.snap) {
    return `<div class="layout"><aside class="sidebar">${sessionCard()}</aside><div class="content"><section class="card"><h2>Comece por aqui</h2><p>Crie uma sessão para ganhar um heap próprio. Cada malloc, free e realloc feito aqui chama a biblioteca em C, e o mapa mostra os blocos exatamente como estão na memória.</p></section></div></div>`;
  }
  const snap = state.snap;
  const check = snap.check.ok ? `<span class="badge ok">heap consistente</span>` : `<span class="badge err">${esc(snap.check.message)}</span>`;
  return `<div class="layout">
    <aside class="sidebar">${sessionCard()}${controlsCard()}</aside>
    <div class="content">
      ${statsRow()}
      <section class="card">
        <div style="display:flex; justify-content:space-between; gap:8px; align-items:center; flex-wrap:wrap; margin-bottom:12px"><h2 style="margin:0">Mapa do heap</h2>${check}</div>
        ${heapMap(snap.arenas, snap.blocks, state.selected)}
        ${legend()}
        ${snap.total_blocks > snap.blocks.length ? `<p class="hint">Mostrando ${snap.blocks.length} de ${snap.total_blocks} blocos.</p>` : ""}
      </section>
      ${detailCard()}
      <div class="split">${freeListCard()}${treeCard()}</div>
      ${allocTable()}
      ${opsTable()}
    </div>
  </div>`;
}

function compareView(): string {
  const result = state.compare;
  const cards = result
    ? result.policies
        .map((p) => {
          const s = p.stats;
          return `<section class="card mini">
            <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:10px"><h2 style="margin:0">${p.policy}</h2>${p.check.ok ? '<span class="badge ok">ok</span>' : `<span class="badge err">${esc(p.check.message)}</span>`}</div>
            ${heapMap(p.arenas, p.blocks, null, true)}
            <div class="table-wrap" style="margin-top:12px"><table><tbody>
              <tr><td>Pico de memória</td><td class="num">${bytes(s.peak_arena_bytes)}</td></tr>
              <tr><td>Passos de busca</td><td class="num">${num(s.search_steps)}</td></tr>
              <tr><td>Fragmentação final</td><td class="num">${pct(s.fragmentation)}</td></tr>
              <tr><td>Divisões / fusões</td><td class="num">${num(s.splits)} / ${num(s.coalesces)}</td></tr>
              <tr><td>Falhas</td><td class="num">${p.result.failed}</td></tr>
              <tr><td>Tempo</td><td class="num">${(p.seconds * 1000).toFixed(1).replace(".", ",")} ms</td></tr>
            </tbody></table></div>
          </section>`;
        })
        .join("")
    : "";
  return `<div class="layout">
    <aside class="sidebar">
      <section class="card">
        <h2>Mesmo trace, três algoritmos</h2>
        <form data-form="compare" style="display:flex; flex-direction:column; gap:8px">
          <textarea name="content" spellcheck="false" aria-label="Trace para comparar">${esc(state.compareText || SAMPLE)}</textarea>
          <button class="btn" type="submit">Comparar</button>
        </form>
        <h3>Gerar trace aleatório</h3>
        <form data-form="generate" class="row">
          <label>Comandos<input name="operations" type="number" min="1" max="50000" value="2000"></label>
          <label>Semente<input name="seed" type="number" min="0" value="42"></label>
          <label>Até (bytes)<input name="max_size" type="number" min="1" value="4096"></label>
          <button class="btn ghost" type="submit">Gerar</button>
        </form>
      </section>
    </aside>
    <div class="content">${result ? `<p class="hint">${num(result.commands)} comandos executados em heaps novos, um por algoritmo.</p><div class="compare">${cards}</div>` : `<section class="card"><p class="empty">Rode um trace para ver lado a lado como First-Fit, Next-Fit e Best-Fit organizam a mesma sequência de pedidos.</p></section>`}</div>
  </div>`;
}

function benchView(): string {
  const last = state.benches[0];
  const max = last ? Math.max(...last.results.map((r) => r.ops_per_second)) : 1;
  const bars = last
    ? last.results
        .map((r) => `<div class="line"><span>${esc(r.allocator)}</span><div class="track"><div class="fill ${r.allocator.startsWith("malloc") ? "sys" : ""}" style="width:${(r.ops_per_second / max) * 100}%"></div></div><span class="value">${num(r.ops_per_second)} ops/s</span></div>`)
        .join("")
    : "";
  const table = last
    ? `<div class="table-wrap" style="margin-top:16px"><table><thead><tr><th>Alocador</th><th class="num">ops/s</th><th class="num">Tempo</th><th class="num">Pico</th><th class="num">Fragm.</th><th class="num">Aprov.</th><th class="num">Passos</th></tr></thead><tbody>${last.results
        .map((r) => `<tr><td>${esc(r.allocator)}</td><td class="num">${num(r.ops_per_second)}</td><td class="num">${r.seconds.toFixed(3).replace(".", ",")} s</td><td class="num">${r.peak_arena_bytes ? bytes(r.peak_arena_bytes) : "-"}</td><td class="num">${r.peak_arena_bytes ? pct(r.fragmentation) : "-"}</td><td class="num">${r.peak_arena_bytes ? pct(r.utilization) : "-"}</td><td class="num">${r.peak_arena_bytes ? num(r.search_steps) : "-"}</td></tr>`)
        .join("")}</tbody></table></div>`
    : "";
  const history = state.benches
    .map((b) => `<tr><td>#${b.id}</td><td class="num">${num(b.operations)}</td><td class="num">${b.seed}</td><td class="num">${b.min_size}–${b.max_size}</td><td>${new Date(b.created_at * 1000).toLocaleString("pt-BR")}</td></tr>`)
    .join("");
  return `<div class="layout">
    <aside class="sidebar"><section class="card">
      <h2>Carga aleatória</h2>
      <form data-form="bench" style="display:flex; flex-direction:column; gap:8px">
        <div class="row"><label>Operações<input name="operations" type="number" min="1000" max="5000000" value="300000"></label><label>Semente<input name="seed" type="number" min="0" value="42"></label></div>
        <div class="row"><label>Mínimo (B)<input name="min_size" type="number" min="1" value="16"></label><label>Máximo (B)<input name="max_size" type="number" min="1" value="4096"></label></div>
        <button class="btn" type="submit">Rodar benchmark</button>
      </form>
      <p class="hint">Roda em C, dentro da biblioteca, sem passar pelo Python a cada operação. O malloc do sistema entra como referência.</p>
    </section></aside>
    <div class="content">
      <section class="card"><h2>Resultado</h2>${last ? bars + table : `<p class="empty">Nenhum benchmark ainda.</p>`}</section>
      <section class="card"><h2>Execuções anteriores</h2>${history ? `<div class="table-wrap"><table><thead><tr><th>Execução</th><th class="num">Operações</th><th class="num">Semente</th><th class="num">Tamanhos</th><th>Data</th></tr></thead><tbody>${history}</tbody></table></div>` : `<p class="empty">Vazio.</p>`}</section>
    </div>
  </div>`;
}

function tracesView(): string {
  const rows = state.traces
    .map((t) => `<tr><td>${esc(t.name)}</td><td class="num">${num(t.commands)}</td><td>${new Date(t.updated_at * 1000).toLocaleString("pt-BR")}</td><td><button class="btn ghost" type="button" data-trace-open="${t.id}">Abrir</button> <button class="btn ghost" type="button" data-trace-compare="${t.id}">Comparar</button> <button class="btn danger" type="button" data-trace-delete="${t.id}">Excluir</button></td></tr>`)
    .join("");
  return `<div class="layout">
    <aside class="sidebar"><section class="card">
      <h2>Editor</h2>
      <form data-form="save-trace" style="display:flex; flex-direction:column; gap:8px">
        <label>Nome<input name="name" maxlength="80" required pattern="[\\w .\\-]+" value="${esc(state.traceEdit.name)}"></label>
        <textarea name="content" spellcheck="false" aria-label="Conteúdo do trace">${esc(state.traceEdit.content || SAMPLE)}</textarea>
        <button class="btn" type="submit">Salvar</button>
      </form>
    </section></aside>
    <div class="content"><section class="card"><h2>Traces salvos</h2>${rows ? `<div class="table-wrap"><table><thead><tr><th>Nome</th><th class="num">Comandos</th><th>Atualizado</th><th>Ações</th></tr></thead><tbody>${rows}</tbody></table></div>` : `<p class="empty">Nenhum trace salvo.</p>`}</section></div>
  </div>`;
}

function connect(id: string) {
  state.socket?.close();
  state.live = false;
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws/sessions/${id}`);
  state.socket = ws;
  ws.onopen = () => {
    state.live = true;
  };
  ws.onmessage = async (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "snapshot" && state.sessionId === id) {
      state.snap = msg.data;
      state.ops = await api.operations(id).catch(() => state.ops);
      if (state.view === "heap") renderView();
    }
  };
  ws.onclose = () => {
    if (state.socket === ws) {
      state.live = false;
      setTimeout(() => state.sessionId === id && state.socket === ws && connect(id), 2000);
    }
  };
}

async function selectSession(id: string | null) {
  state.sessionId = id;
  state.selected = null;
  state.dump = null;
  state.snap = null;
  if (!id) {
    state.socket?.close();
    renderView();
    return;
  }
  state.snap = (await guard(() => api.snapshot(id))) ?? null;
  state.ops = (await guard(() => api.operations(id))) ?? [];
  connect(id);
  renderView();
}

async function refresh() {
  if (!state.sessionId) return;
  const snap = await guard(() => api.snapshot(state.sessionId!));
  if (snap) state.snap = snap;
  state.ops = (await guard(() => api.operations(state.sessionId!))) ?? state.ops;
  renderView();
}

function formData(form: HTMLFormElement): Record<string, string> {
  return Object.fromEntries(new FormData(form).entries()) as Record<string, string>;
}

async function onSubmit(form: HTMLFormElement) {
  const kind = form.dataset.form!;
  const d = formData(form);
  const id = state.sessionId;
  if (kind === "session") {
    const s = await guard(() => api.createSession({ name: d.name, order: d.order, grow_size: Number(d.grow_size), mmap_threshold: Number(d.mmap_threshold) }));
    if (s) {
      state.sessions = await api.sessions();
      await selectSession(s.id);
    }
    return;
  }
  if (kind === "compare") {
    state.compareText = d.content;
    const r = await guard(() => api.compare(d.content));
    if (r) state.compare = r;
    renderView();
    return;
  }
  if (kind === "generate") {
    const g = await guard(() => api.generate(Number(d.operations), Number(d.seed), 16, Number(d.max_size)));
    if (g) {
      state.compareText = g.content;
      renderView();
    }
    return;
  }
  if (kind === "bench") {
    const b = await guard(() => api.bench(Number(d.operations), Number(d.seed), Number(d.min_size), Number(d.max_size)));
    if (b) state.benches = (await api.benches()) ?? [b];
    renderView();
    return;
  }
  if (kind === "save-trace") {
    const t = await guard(() => api.saveTrace(d.name, d.content));
    if (t) {
      toast(`Trace "${t.name}" salvo`);
      state.traceEdit = { name: t.name, content: d.content };
      state.traces = await api.traces();
      renderView();
    }
    return;
  }
  if (!id) return;
  if (kind === "alloc") {
    const a = await guard(() => api.alloc(id, Number(d.size), d.label));
    if (a) state.selected = a.address;
  } else if (kind === "calloc") {
    const a = await guard(() => api.calloc(id, Number(d.count), Number(d.size), ""));
    if (a) state.selected = a.address;
  } else if (kind === "memalign") {
    const a = await guard(() => api.memalign(id, Number(d.alignment), Number(d.size), ""));
    if (a) state.selected = a.address;
  } else if (kind === "realloc") {
    const cur = state.snap?.allocations.find((x) => x.address === state.selected);
    if (!cur) return;
    const a = await guard(() => api.realloc(id, cur.handle, Number(d.size)));
    if (a) {
      state.selected = a.address;
      if (a.address !== cur.address) toast(`Bloco movido para ${a.address_hex}`);
    }
  } else if (kind === "write") {
    const cur = state.snap?.allocations.find((x) => x.address === state.selected);
    if (!cur) return;
    await guard(() => api.write(id, cur.handle, d.text));
    state.dump = (await guard(() => api.read(id, cur.handle))) ?? null;
  } else if (kind === "trace") {
    const r = await guard(() => api.runTrace(id, d.content));
    if (r) toast(`${r.commands} comandos executados · ${r.failed} falhas · ${r.ignored} ignorados`);
  }
  await refresh();
}

async function onClick(target: HTMLElement) {
  const el = target.closest<HTMLElement>("[data-view],[data-action],[data-policy],[data-quick],[data-address],[data-select],[data-free],[data-trace-open],[data-trace-compare],[data-trace-delete]");
  if (!el) return;
  const id = state.sessionId;
  if (el.dataset.view) {
    state.view = el.dataset.view as View;
    if (state.view === "bench") state.benches = (await guard(() => api.benches())) ?? [];
    if (state.view === "traces") state.traces = (await guard(() => api.traces())) ?? [];
    render();
    return;
  }
  if (el.dataset.action === "theme") {
    const root = document.documentElement;
    const dark = root.dataset.theme ? root.dataset.theme === "dark" : matchMedia("(prefers-color-scheme: dark)").matches;
    root.dataset.theme = dark ? "light" : "dark";
    try {
      localStorage.setItem("favo-tema", root.dataset.theme);
    } catch {
      return;
    }
    return;
  }
  if (el.dataset.traceOpen || el.dataset.traceCompare) {
    const t = await guard(() => api.trace(Number(el.dataset.traceOpen || el.dataset.traceCompare)));
    if (!t) return;
    if (el.dataset.traceCompare) {
      state.compareText = t.content ?? "";
      state.view = "compare";
      render();
    } else {
      state.traceEdit = { name: t.name, content: t.content ?? "" };
      renderView();
    }
    return;
  }
  if (el.dataset.traceDelete) {
    await guard(() => api.deleteTrace(Number(el.dataset.traceDelete)));
    state.traces = (await guard(() => api.traces())) ?? [];
    renderView();
    return;
  }
  if (!id) return;
  if (el.dataset.policy) {
    await guard(() => api.policy(id, el.dataset.policy as Policy));
    state.sessions = (await guard(() => api.sessions())) ?? state.sessions;
  } else if (el.dataset.quick) {
    const a = await guard(() => api.alloc(id, Number(el.dataset.quick), ""));
    if (a) state.selected = a.address;
  } else if (el.dataset.address || el.dataset.select) {
    state.selected = Number(el.dataset.address || el.dataset.select);
    state.dump = null;
    renderView();
    return;
  } else if (el.dataset.free) {
    await guard(() => api.free(id, Number(el.dataset.free)));
    state.selected = null;
    state.dump = null;
  } else if (el.dataset.action === "read") {
    const cur = state.snap?.allocations.find((x) => x.address === state.selected);
    if (cur) state.dump = (await guard(() => api.read(id, cur.handle))) ?? null;
  } else if (el.dataset.action === "free-all") {
    await guard(() => api.freeAll(id));
    state.selected = null;
  } else if (el.dataset.action === "trim") {
    const r = await guard(() => api.trim(id));
    if (r) toast(`${bytes(r.released)} devolvidos ao sistema`);
  } else if (el.dataset.action === "reset") {
    await guard(() => api.reset(id));
    state.selected = null;
  }
  await refresh();
}

app.addEventListener("submit", (ev) => {
  const form = ev.target as HTMLFormElement;
  if (!form.dataset.form) return;
  ev.preventDefault();
  void onSubmit(form);
});

app.addEventListener("click", (ev) => {
  void onClick(ev.target as HTMLElement);
});

app.addEventListener("keydown", (ev) => {
  const el = ev.target as HTMLElement;
  if (ev.key === "Enter" && el.dataset.select) void onClick(el);
});

app.addEventListener("change", (ev) => {
  const el = ev.target as HTMLSelectElement;
  if (el.dataset.change === "session") void selectSession(el.value);
});

async function boot() {
  render();
  state.sessions = (await guard(() => api.sessions())) ?? [];
  if (state.sessions.length) await selectSession(state.sessions[0].id);
  else renderView();
}

void boot();
