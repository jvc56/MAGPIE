import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";
import vm from "node:vm";

async function setupHarness(state = "activated") {
  const source = await readFile("isolation.mjs", "utf8");
  const scriptURL = "https://example.test/wasmentry/isolation-worker.js";
  const worker = Object.assign(new EventTarget(), { state, scriptURL });
  const registration = Object.assign(new EventTarget(), {
    active: state === "activated" ? worker : null,
    installing: state === "activated" ? null : worker,
    waiting: null,
  });
  const serviceWorker = Object.assign(new EventTarget(), {
    controller: null,
    register: async () => registration,
  });
  const store = new Map();
  let reloads = 0;
  const timers = new Set();
  const window = {};
  window.self = window;
  window.top = window;
  const context = {
    URL,
    window,
    isSecureContext: true,
    crossOriginIsolated: false,
    navigator: { serviceWorker },
    sessionStorage: {
      getItem: (key) => store.get(key),
      setItem: (key, value) => store.set(key, value),
      removeItem: (key) => store.delete(key),
    },
    location: { href: "https://example.test/wasmentry/", reload: () => reloads++ },
    history: { replaceState: () => {} },
    setTimeout: (fn, ms) => {
      const timer = setTimeout(fn, ms);
      timers.add(timer);
      return timer;
    },
    clearTimeout: (timer) => {
      clearTimeout(timer);
      timers.delete(timer);
    },
  };
  const run = () =>
    vm.runInNewContext(
      source
        .replace("export async function", "async function")
        .replaceAll(
          "import.meta.url",
          JSON.stringify("https://example.test/wasmentry/isolation.mjs"),
        ) + "\nensureIsolation();",
      context,
    );
  return {
    run,
    context,
    worker,
    registration,
    reloads: () => reloads,
    cleanup: () => {
      for (const timer of timers) clearTimeout(timer);
    },
  };
}

test("an active worker without a controller reloads instead of timing out", async () => {
  const harness = await setupHarness();
  try {
    expect(
      await Promise.race([
        harness.run(),
        new Promise((resolve) => setTimeout(() => resolve("stuck"), 200)),
      ]),
    ).toBe(false);
    expect(harness.reloads()).toBe(1);
    const retryError = await harness.run().then(
      () => "",
      (error) => error.message,
    );
    expect(retryError).toContain("could not enable shared memory");
    expect(harness.reloads()).toBe(1);
  } finally {
    harness.cleanup();
  }
});

test("fresh installation waits for activation even without controllerchange", async () => {
  const harness = await setupHarness("installing");
  try {
    const ready = harness.run();
    await new Promise((resolve) => setTimeout(resolve, 0));
    expect(harness.reloads()).toBe(0);
    harness.worker.state = "activated";
    harness.registration.active = harness.worker;
    harness.registration.installing = null;
    harness.worker.dispatchEvent(new Event("statechange"));
    expect(
      await Promise.race([
        ready,
        new Promise((resolve) => setTimeout(() => resolve("stuck"), 200)),
      ]),
    ).toBe(false);
    expect(harness.reloads()).toBe(1);
  } finally {
    harness.cleanup();
  }
});

test("plain static hosting starts the real engine and never requests WMP or RIT", async ({
  page,
  context,
}) => {
  const urls = [];
  context.on("request", (request) => urls.push(request.url()));
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  expect(await page.evaluate(() => crossOriginIsolated)).toBe(true);
  await page.locator('[data-mode="kibitz"]').click();
  await expect(page.locator("#status")).toHaveText("Complete");
  expect(urls.some((url) => /\.kwg(?:\?|$)/.test(url))).toBe(true);
  expect(urls.some((url) => /\.(wmp|rit)(?:\?|$)/.test(url))).toBe(false);
  // Simulate a force reload bypassing an already-installed worker for the
  // initial navigation, then allow the automatic recovery navigation.
  const devtools = await context.newCDPSession(page);
  await devtools.send("Network.setBypassServiceWorker", { bypass: true });
  await page.goto("/wasmentry/", { waitUntil: "commit" });
  await devtools.send("Network.setBypassServiceWorker", { bypass: false });
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 10000,
  });
  await devtools.detach();
  const second = await context.newPage();
  await second.goto("/wasmentry/");
  await expect(second.locator("#status")).toHaveText("Engine ready");
  await second.reload();
  await expect(second.locator("#status")).toHaveText("Engine ready");
});

 test("blocked storage does not break isolated startup or trap recovery", async () => {
   const h=await setupHarness();
   h.context.sessionStorage={getItem(){throw Error("blocked")},setItem(){throw Error("blocked")},removeItem(){throw Error("blocked")}};
   h.context.history.replaceState=(_a,_b,url)=>{h.context.location.href=String(url)};
   h.context.crossOriginIsolated=true;
   expect(await h.run()).toBe(true);
   h.context.crossOriginIsolated=false;
   expect(await h.run()).toBe(false);
   expect(await h.run().catch(e=>e.message)).toContain("could not enable shared memory");
   expect(h.reloads()).toBe(1);
   expect(await h.run()).toBe(false);
   expect(h.reloads()).toBe(2);
   h.cleanup();
 });
