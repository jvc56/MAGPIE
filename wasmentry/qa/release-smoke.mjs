// Run against an unpacked release or its deployed URL, never a source checkout.
// Service workers are blocked to prove the server itself enables WASM threads.
import assert from "node:assert/strict";
import { chromium, expect } from "@playwright/test";

const url = process.argv[2];
assert(url, "Usage: node wasmentry/qa/release-smoke.mjs URL");
const browser = await chromium.launch();
try {
  const context = await browser.newContext({ serviceWorkers: "block" });
  const page = await context.newPage();
  const requests = [];
  const errors = [];
  context.on("request", (request) => requests.push(request.url()));
  page.on("pageerror", (error) => errors.push(error.message));
  let navigations = 0;
  page.on("framenavigated", (frame) => {
    if (frame === page.mainFrame()) navigations++;
  });
  const response = await page.goto(url);
  assert.equal(response.status(), 200);
  assert.equal(response.headers()["cross-origin-opener-policy"], "same-origin");
  assert.equal(response.headers()["cross-origin-embedder-policy"], "require-corp");
  await expect(page.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  assert.equal(await page.evaluate(() => crossOriginIsolated), true);
  assert.equal(navigations, 1, "Startup must not reload to install a service worker");
  await page.locator("#open-analysis-settings").click();
  await expect(page.locator('#wmp-source option[value="download"]')).toHaveCount(0);
  await expect(page.locator("#wmp-cache-enabled")).not.toBeChecked();
  await page.locator("#wmp-source").selectOption("build");
  await page.locator("#seconds").fill("1");
  await page.locator("#threads").fill("2");
  await page.locator("#back-to-game").click();
  for (const [sample, mode] of [["opening", "kibitz"], ["midgame", "sim"], ["peg", "peg"], ["endgame", "endgame"]]) {
    if (!(await page.locator("#sample").isVisible()))
      await page.locator("#position-editor > summary").click();
    await page.selectOption("#sample", sample);
    await page.locator(`[data-mode="${mode}"]`).click();
    await expect(page.locator("#status")).toHaveText("Complete", { timeout: 60000 });
    await expect(page.locator("#results tbody tr").first()).toBeVisible();
    await expect(page.locator("#error")).toBeEmpty();
    console.log(`${mode}: complete`);
  }
  await page.reload();
  await expect(page.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  const second = await context.newPage();
  await second.goto(url);
  await expect(second.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  assert.equal(await second.evaluate(() => crossOriginIsolated), true);
  assert(!requests.some((request) => /\.(rit|wit|wmp)(?:\?|$)|\/data\/wmp/.test(request)));
  assert(!requests.some((request) => request.endsWith("/isolation-worker.js")));
  assert(requests.some((request) => request.endsWith(".kwg")));
  assert.deepEqual(errors, []);
  console.log("Headers, local word-map building, all four searches, reload and second tab passed without hosted tables or service workers.");
} finally {
  await browser.close();
}
