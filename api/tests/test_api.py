def new_session(client, policy="first-fit"):
    r = client.post("/api/sessions", json={"name": "teste", "policy": policy})
    assert r.status_code == 201
    return r.json()["id"]


def test_health_and_info(client):
    assert client.get("/api/health").json()["status"] == "ok"
    info = client.get("/api/info").json()
    assert info["alignment"] == 16
    assert "best-fit" in info["policies"]


def test_alloc_write_read_free(client):
    sid = new_session(client)
    a = client.post(f"/api/sessions/{sid}/alloc", json={"size": 100, "label": "buffer"}).json()
    assert a["address"] % 16 == 0
    assert a["usable"] >= 100
    w = client.post(f"/api/sessions/{sid}/write", json={"handle": a["handle"], "text": "olá favo"})
    assert w.status_code == 200
    r = client.get(f"/api/sessions/{sid}/read/{a['handle']}").json()
    assert r["text"] == "olá favo"
    snap = client.get(f"/api/sessions/{sid}/heap").json()
    assert snap["check"]["ok"]
    assert any(b["handle"] == a["handle"] for b in snap["blocks"])
    assert client.post(f"/api/sessions/{sid}/free", json={"handle": a["handle"]}).status_code == 200
    assert client.post(f"/api/sessions/{sid}/free", json={"handle": a["handle"]}).status_code == 404
    ops = client.get(f"/api/sessions/{sid}/operations").json()
    assert [o["kind"] for o in ops][:3] == ["free", "write", "alloc"]


def test_realloc_memalign_calloc_and_policy(client):
    sid = new_session(client, "best-fit")
    a = client.post(f"/api/sessions/{sid}/alloc", json={"size": 64}).json()
    r = client.post(f"/api/sessions/{sid}/realloc", json={"handle": a["handle"], "size": 5000}).json()
    assert r["usable"] >= 5000
    m = client.post(f"/api/sessions/{sid}/memalign", json={"alignment": 4096, "size": 10}).json()
    assert m["address"] % 4096 == 0
    c = client.post(f"/api/sessions/{sid}/calloc", json={"count": 10, "size": 10}).json()
    assert client.get(f"/api/sessions/{sid}/read/{c['handle']}?length=100").json()["hex"].replace(" ", "") == "00" * 100
    assert client.post(f"/api/sessions/{sid}/memalign", json={"alignment": 48, "size": 10}).status_code == 422
    assert client.put(f"/api/sessions/{sid}/policy", json={"policy": "next-fit"}).json()["policy"] == "next-fit"
    snap = client.get(f"/api/sessions/{sid}/heap").json()
    assert snap["check"]["ok"]
    assert len(snap["tree"]) == snap["stats"]["free_blocks"]
    assert len(snap["free_list"]) == snap["stats"]["free_blocks"]


def test_budget_and_reset(client):
    r = client.post("/api/sessions", json={"name": "pequena", "max_bytes": 65536, "grow_size": 16384, "mmap_threshold": 0})
    sid = r.json()["id"]
    codes = [client.post(f"/api/sessions/{sid}/alloc", json={"size": 4000}).status_code for _ in range(40)]
    assert 409 in codes
    assert client.post(f"/api/sessions/{sid}/reset").status_code == 200
    assert client.get(f"/api/sessions/{sid}/heap").json()["stats"]["arena_count"] == 0


def test_traces_compare_and_benchmark(client):
    gen = client.post("/api/traces/generate", json={"operations": 300, "seed": 3}).json()
    saved = client.post("/api/traces", json={"name": "aleatorio", "content": gen["content"]})
    assert saved.status_code == 201
    assert client.get("/api/traces").json()[0]["name"] == "aleatorio"
    cmp = client.post("/api/compare", json={"content": gen["content"]}).json()
    assert [p["policy"] for p in cmp["policies"]] == ["first-fit", "next-fit", "best-fit"]
    assert all(p["check"]["ok"] for p in cmp["policies"])
    assert client.post("/api/compare", json={"content": "x 1"}).status_code == 422
    sid = new_session(client)
    res = client.post(f"/api/sessions/{sid}/trace", json={"content": "a 1 100\na 2 200\nf 1\n"}).json()
    assert res["commands"] == 3
    bench = client.post("/api/benchmarks", json={"operations": 5000}).json()
    assert len(bench["results"]) == 4
    assert client.get(f"/api/benchmarks/{bench['id']}").status_code == 200


def test_websocket_pushes_snapshots(client):
    sid = new_session(client)
    with client.websocket_connect(f"/ws/sessions/{sid}") as ws:
        first = ws.receive_json()
        assert first["type"] == "snapshot"
        client.post(f"/api/sessions/{sid}/alloc", json={"size": 300})
        update = ws.receive_json()
        assert update["data"]["stats"]["used_blocks"] == 1
        ws.send_text("ping")
        assert ws.receive_json()["type"] == "pong"


def test_session_lifecycle(client):
    sid = new_session(client)
    assert client.get(f"/api/sessions/{sid}").status_code == 200
    assert client.delete(f"/api/sessions/{sid}").status_code == 204
    assert client.get(f"/api/sessions/{sid}").status_code == 404
    assert client.get("/api/sessions/history").json()[0]["closed_at"] is not None
