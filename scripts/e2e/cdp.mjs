// Shared by the end-to-end scripts (e2e-desk.mjs, e2e-tournament.mjs): a
// Chrome driven over the DevTools protocol (no Puppeteer), the run's steps
// and the layout checks. Pictures go to the folder chrome() is given.
import { spawn } from "node:child_process";
import { writeFileSync } from "node:fs";
import { join } from "node:path";

// --- Chrome over the DevTools protocol.

export async function chrome(out) {
  const up = () => fetch("http://127.0.0.1:9222/json/version").then((r) => r.ok).catch(() => false);
  if (!(await up())) {
    spawn("powershell.exe", ["-NoProfile", "-Command",
      "Start-Process -FilePath 'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe' -ArgumentList '--headless=new','--remote-debugging-port=9222','--user-data-dir=C:\\Users\\Public\\ember-e2e-chrome','--no-first-run','about:blank'"],
    { stdio: "ignore", detached: true }).unref();
    for (let i = 0; i < 40 && !(await up()); i++) await sleep(500);
  }
  const target = await (await fetch("http://127.0.0.1:9222/json/new?about:blank", { method: "PUT" })).json();
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.onopen = resolve; socket.onerror = reject; });
  let next = 0;
  const waiting = new Map();
  socket.onmessage = (message) => {
    const data = JSON.parse(message.data);
    if (data.id && waiting.has(data.id)) {
      const [resolve, reject] = waiting.get(data.id);
      waiting.delete(data.id);
      data.error ? reject(new Error(data.error.message)) : resolve(data.result);
    }
  };
  const send = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++next;
    waiting.set(id, [resolve, reject]);
    socket.send(JSON.stringify({ id, method, params }));
  });
  await send("Page.enable");
  await send("Runtime.enable");
  // Signed out at the start, whatever an earlier run left.
  await send("Network.clearBrowserCookies");
  return {
    send,
    close: () => { socket.close(); fetch("http://127.0.0.1:9222/json/close/" + target.id).catch(() => {}); },
    async eval(expression) {
      const result = await send("Runtime.evaluate", { expression, awaitPromise: true, returnByValue: true });
      if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text);
      return result.result.value;
    },
    async go(url) {
      await send("Page.navigate", { url });
      await this.until("document.readyState === 'complete'");
      // Confirms answer yes, as the organizer would.
      await this.eval("window.confirm = () => true");
    },
    async until(expression, ms = 10000) {
      const end = Date.now() + ms;
      for (;;) {
        if (await this.eval(`Boolean(${expression})`).catch(() => false)) return;
        if (Date.now() > end) throw new Error("timed out waiting for " + expression);
        await sleep(200);
      }
    },
    async width(px) {
      await send("Emulation.setDeviceMetricsOverride", { width: px, height: px < 600 ? 844 : px >= 1920 ? 1080 : 900, deviceScaleFactor: 1, mobile: px < 600 });
    },
    async shot(name) {
      const { data } = await send("Page.captureScreenshot", { format: "png" });
      writeFileSync(join(out, name + ".png"), Buffer.from(data, "base64"));
    },
  };
}

export const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
export const results = [];
export async function step(name, run) {
  try {
    await run();
    results.push(["ok", name]);
    console.log("ok  ", name);
  } catch (error) {
    results.push(["FAIL", name, error.message]);
    console.log("FAIL", name, "-", error.message);
  }
}
export function must(condition, text) {
  if (!condition) throw new Error(text);
}

/** At each width: nothing scrolls sideways, and every shown button is 44px
 *  tall; with `fits`, the page fits a desktop screen without scrolling. */
export async function layout(page, label, fits = false) {
  for (const px of [390, 1440, 1920]) {
    await page.width(px);
    await sleep(200);
    const check = await page.eval(`(() => {
      const wide = document.documentElement.scrollWidth > innerWidth + 1;
      const tall = document.documentElement.scrollHeight > innerHeight + 1;
      const empty = /\b(null|undefined|NaN)\b/.test(document.body.innerText);
      const small = [...document.querySelectorAll("button, a.wd-btn, .chip")]
        .filter((b) => b.offsetParent && b.getBoundingClientRect().height < 44)
        .map((b) => (b.textContent || b.getAttribute("aria-label") || "").trim().slice(0, 30));
      return { wide, tall, small, empty };
    })()`);
    await page.shot(label + "-" + px);
    must(!check.wide, label + " scrolls sideways at " + px + "px");
    must(!check.empty, label + " shows null, undefined or NaN");
    must(!check.small.length, label + " has buttons under 44px at " + px + "px: " + check.small.join(", "));
    must(!(fits && px >= 1440 && check.tall), label + " scrolls at " + px + "px wide");
  }
}

