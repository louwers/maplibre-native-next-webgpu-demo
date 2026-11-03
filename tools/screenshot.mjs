// Opens the demo in headless Chromium with WebGPU, prints its console, and
// screenshots it after a delay. A directory is served locally with the
// cross-origin isolation headers that web/_headers sets on Workers.
//
// Usage: node tools/screenshot.mjs <web directory | URL> <out.png> [seconds] [JS to run after load]
// The browser is $CHROME, else /run/current-system/sw/bin/chromium.
import { spawn } from "node:child_process";
import { createReadStream, existsSync, statSync, writeFileSync, mkdtempSync } from "node:fs";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import path from "node:path";

const [target, out, seconds = "20", script = ""] = process.argv.slice(2);
const dir = /^https?:/.test(target) ? null : target;
const types = { ".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm" };
const server = createServer((req, res) => {
  const file = path.join(dir, decodeURIComponent(new URL(req.url, "http://x").pathname));
  const target = existsSync(file) && statSync(file).isDirectory() ? path.join(file, "index.html") : file;
  if (!existsSync(target)) { res.writeHead(404); res.end(); return; }
  res.writeHead(200, {
    "Content-Type": types[path.extname(target)] ?? "application/octet-stream",
    "Cross-Origin-Opener-Policy": "same-origin",
    "Cross-Origin-Embedder-Policy": "credentialless",
  });
  createReadStream(target).pipe(res);
});
let url = target;
if (dir) {
  await new Promise((r) => server.listen(0, "127.0.0.1", r));
  url = `http://127.0.0.1:${server.address().port}/`;
}
const env = { ...process.env }; delete env.LD_LIBRARY_PATH;
const chrome = spawn(process.env.CHROME ?? "/run/current-system/sw/bin/chromium", [
  "--headless=new", "--no-sandbox", "--remote-debugging-port=0", `--user-data-dir=${mkdtempSync(path.join(tmpdir(), "shot-"))}`,
  "--enable-unsafe-webgpu", "--enable-features=Vulkan", "--enable-unsafe-swiftshader", "--use-angle=swiftshader", "--use-vulkan=swiftshader",
  "--window-size=1024,700", "about:blank",
], { env, stdio: ["ignore", "ignore", "pipe"] });
const wsUrl = await new Promise((resolve) => {
  chrome.stderr.on("data", (d) => { const m = /DevTools listening on (ws:\S+)/.exec(String(d)); if (m) resolve(m[1]); });
});
const targets = await (await fetch(wsUrl.replace("ws://", "http://").replace(/\/devtools.*/, "/json/list"))).json();
const page = targets.find((t) => t.type === "page");
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((r) => ws.addEventListener("open", r));
let id = 0; const pending = new Map();
ws.addEventListener("message", (e) => {
  const msg = JSON.parse(e.data);
  if (msg.id && pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); }
  if (msg.method === "Runtime.consoleAPICalled") console.log("[console]", msg.params.args.map((a) => a.value ?? a.description).join(" "));
  if (msg.method === "Runtime.exceptionThrown") console.log("[exception]", msg.params.exceptionDetails.exception?.description ?? msg.params.exceptionDetails.text);
});
const send = (method, params = {}) => new Promise((r) => { const i = ++id; pending.set(i, r); ws.send(JSON.stringify({ id: i, method, params })); });
await send("Runtime.enable");
await send("Page.enable");
await send("Page.navigate", { url });
await new Promise((r) => setTimeout(r, 3000));
if (script) console.log("[eval]", JSON.stringify((await send("Runtime.evaluate", { expression: script, awaitPromise: true })).result?.result?.value));
await new Promise((r) => setTimeout(r, Number(seconds) * 1000));
const status = await send("Runtime.evaluate", { expression: "document.getElementById('status')?.textContent" });
console.log("[status]", status.result?.result?.value);
const shot = await send("Page.captureScreenshot", { format: "png" });
writeFileSync(out, Buffer.from(shot.result.data, "base64"));
console.log("[screenshot]", out);
chrome.kill();
if (dir) server.close();
process.exit(0);
