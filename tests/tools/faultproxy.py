#!/usr/bin/env python3
"""Fault-injecting TCP proxy for the ks3fs network tests.

Forwards each listener to an upstream S3 server; listeners can terminate TLS
(so the guest's kTLS client talks to a real TLS endpoint).  Faults are
switched at run time over a tiny HTTP control API, which the guest drives
with busybox wget:

  GET /set?mode=normal            forward normally
  GET /set?mode=blackhole         stall all traffic (new and existing conns)
  GET /set?mode=reset             RST every connection, now and new ones
  GET /set?mode=refuse            stop listening (connection refused)
  GET /cut?dir=down&bytes=N&count=K[&port=P]
                                  kill each of the next K connections after
                                  N bytes server->client (dir=up: client->server);
                                  a connection that closes before N bytes hands
                                  its budget on (servers that close after every
                                  response, like versitygw, still get K cuts)
  GET /uncut                      drop the cut budgets not used yet
  GET /slow?bps=N                 throttle server->client to N bytes/s (0=off)
  GET /delay?ms=N                 add N ms before each server->client read
  GET /stats                      JSON counters

  faultproxy.py --upstream 127.0.0.1:9000 --listen 7001 \
      --listen 7002:cert.pem:key.pem --control 7000
"""
import argparse
import asyncio
import json
import socket
import ssl
import struct
import sys
import urllib.parse

state = {"mode": "normal", "bps": 0, "delay": 0.0}
cuts = {"down": [], "up": []}     # queued (bytes, port) budgets for new connections
stats = {"conns": 0, "resets": 0, "cuts": 0, "bytes_down": 0, "bytes_up": 0}
active = {}                       # conn -> {"down": budget|None, "up": ...}
# a budget is [bytes left, bytes, port or None, fired]
servers = []


def rst(writer):
    """Abort a connection with a TCP RST rather than a FIN."""
    sock = writer.get_extra_info("socket")
    try:
        if sock is not None:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                            struct.pack("ii", 1, 0))
    except OSError:
        pass
    writer.transport.abort()


async def pump(reader, writer, direction, conn):
    try:
        while True:
            while state["mode"] == "blackhole":
                await asyncio.sleep(0.05)
            data = await reader.read(65536)
            if not data:
                break
            if direction == "down" and state["delay"]:
                await asyncio.sleep(state["delay"])     # one-way latency
            while state["mode"] == "blackhole":
                await asyncio.sleep(0.05)
            budget = active.get(conn, {}).get(direction)
            if budget is not None:
                if len(data) >= budget[0]:
                    data = data[:budget[0]]
                    if data:
                        writer.write(data)
                        await writer.drain()
                    budget[3] = True
                    stats["cuts"] += 1
                    raise ConnectionAbortedError("cut")
                budget[0] -= len(data)
            if direction == "down" and state["bps"]:
                for i in range(0, len(data), 4096):
                    writer.write(data[i:i + 4096])
                    await writer.drain()
                    await asyncio.sleep(min(4096, len(data) - i) / state["bps"])
            else:
                writer.write(data)
                await writer.drain()
            stats["bytes_" + direction] += len(data)
    except (ConnectionError, OSError, asyncio.IncompleteReadError, ssl.SSLError):
        for w in conn:          # injected or real failure: abort both sides
            rst(w)
        return
    # orderly EOF: pass it on (TLS transports cannot half-close)
    try:
        if writer.can_write_eof():
            writer.write_eof()
        else:
            writer.close()
    except (OSError, RuntimeError):
        pass


async def handle(creader, cwriter, upstream, lport):
    stats["conns"] += 1
    if state["mode"] == "reset":
        stats["resets"] += 1
        rst(cwriter)
        return
    host, port = upstream
    try:
        sreader, swriter = await asyncio.open_connection(host, port)
    except OSError:
        rst(cwriter)
        return
    conn = (cwriter, swriter)
    def take(d):
        for i, (n, port) in enumerate(cuts[d]):
            if port in (None, lport):
                cuts[d].pop(i)
                return [n, n, port, False]
        return None
    active[conn] = {"down": take("down"), "up": take("up"), "port": lport}
    try:
        await asyncio.gather(pump(sreader, cwriter, "down", conn),
                             pump(creader, swriter, "up", conn))
    finally:
        budgets = active.pop(conn, None) or {}
        for d in ("down", "up"):
            b = budgets.get(d)
            if b and not b[3]:      # never reached: the next connection gets it
                cuts[d].insert(0, (b[1], b[2]))


async def start_listeners(listen, upstream):
    for port, ctx in listen:
        srv = await asyncio.start_server(
            lambda r, w, p=port: handle(r, w, upstream, p), "127.0.0.1", port,
            ssl=ctx, reuse_address=True)
        servers.append((srv, port, ctx))


async def set_mode(mode, listen, upstream):
    state["mode"] = mode
    if mode == "reset":
        for conn in list(active):
            stats["resets"] += 1
            for w in conn:
                rst(w)
    if mode == "refuse":
        # stop listening; don't wait_closed(), which waits for every
        # established (keep-alive) connection to go away
        for srv, _, _ in servers:
            srv.close()
        servers.clear()
    elif not servers:
        await start_listeners(listen, upstream)


async def control(reader, writer, listen, upstream):
    try:
        line = (await reader.readline()).decode()
        while (await reader.readline()) not in (b"\r\n", b"\n", b""):
            pass
        path = line.split()[1] if len(line.split()) > 1 else "/"
        url = urllib.parse.urlparse(path)
        q = dict(urllib.parse.parse_qsl(url.query))
        body = "ok\n"
        if url.path == "/set":
            await set_mode(q.get("mode", "normal"), listen, upstream)
        elif url.path == "/cut":
            # established (keep-alive) connections first, then new ones
            d, n, k = q.get("dir", "down"), int(q["bytes"]), int(q.get("count", 1))
            port = int(q["port"]) if "port" in q else None
            for budgets in active.values():
                if k and budgets[d] is None and port in (None, budgets["port"]):
                    budgets[d] = [n, n, port, False]
                    k -= 1
            cuts[d] += [(n, port)] * k
        elif url.path == "/uncut":
            cuts["down"].clear()
            cuts["up"].clear()
            for budgets in active.values():
                budgets["down"] = budgets["up"] = None
        elif url.path == "/delay":
            state["delay"] = int(q.get("ms", 0)) / 1000.0
        elif url.path == "/slow":
            state["bps"] = int(q.get("bps", 0))
        elif url.path == "/stats":
            body = json.dumps(dict(stats, mode=state["mode"])) + "\n"
        else:
            body = "unknown\n"
        print(f"control: {path}", file=sys.stderr, flush=True)
        writer.write(("HTTP/1.0 200 OK\r\nContent-Length: %d\r\n\r\n%s"
                      % (len(body), body)).encode())
        await writer.drain()
    finally:
        writer.close()


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", required=True)
    ap.add_argument("--listen", action="append", default=[],
                    help="PORT or PORT:CERT:KEY for a TLS listener")
    ap.add_argument("--control", type=int, required=True)
    a = ap.parse_args()
    h, p = a.upstream.rsplit(":", 1)
    upstream = (h, int(p))
    listen = []
    for spec in a.listen:
        parts = spec.split(":")
        ctx = None
        if len(parts) == 3:
            ctx = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
            ctx.load_cert_chain(parts[1], parts[2])
        listen.append((int(parts[0]), ctx))
    await start_listeners(listen, upstream)
    ctl = await asyncio.start_server(
        lambda r, w: control(r, w, listen, upstream), "127.0.0.1", a.control)
    print("faultproxy ready", flush=True)
    async with ctl:
        await ctl.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
