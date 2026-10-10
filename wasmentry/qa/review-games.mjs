import { chromium } from "playwright";
import { mkdir, readFile, writeFile } from "node:fs/promises";
import { resolve } from "node:path";
const args = process.argv.slice(2);
const option = (key, fallback) =>
  args.includes(key) ? args[args.indexOf(key) + 1] : fallback;
const input = resolve(option("--input", "/private/tmp/magpie-blitz-qa"));
const output = resolve(option("--output", "/private/tmp/magpie-review-qa"));
const count = Number(option("--games", "10"));
const url = option("--url", "http://localhost:8000/wasmentry/");
await mkdir(output, { recursive: true });
const browser = await chromium.launch();
const reports = [];
const log = (value) =>
  console.log(JSON.stringify({ time: new Date().toISOString(), ...value }));
for (let number = 1; number <= count; number++) {
  const page = await browser.newPage({
    viewport:
      number % 2
        ? { width: 1440, height: 1100 }
        : { width: 390, height: 844 },
  });
  page.setDefaultTimeout(20000);
  await page.addInitScript(() => {
    window.qaCommands = [];
    const send = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function (message, ...rest) {
      if (message.type === "run")
        window.qaCommands.push(message.data.commands);
      return send.call(this, message, ...rest);
    };
  });
  const report = { game: number, positions: [], issues: [] };
  page.on("pageerror", (e) =>
    report.issues.push({ type: "pageerror", error: e.message }),
  );
  try {
    await page.goto(url);
    await page.waitForFunction(
      () => document.querySelector("#status").textContent === "Engine ready",
    );
    await page
      .locator("#gcg-file")
      .setInputFiles(resolve(input, `game-${number}.gcg`));
    await page.waitForFunction(() =>
      document.querySelector("#status").textContent.startsWith("Loaded"),
    );
    await page.locator("#open-analysis-settings").click();
    await page.locator("#seconds").fill("1");
    await page.locator("#candidates").fill("10");
    await page.locator("#back-to-game").click();
    const gcg = await readFile(resolve(input, `game-${number}.gcg`), "utf8");
    const events = gcg.split(/\r?\n/).filter((line) => line.startsWith(">"));
    const savedPhases = new Set();
    for (let index = 0; index < events.length; index++) {
      // Analyze decisions, not final score adjustments.
      if (
        !/^>[^:]+:\s+\S+\s+(?:[A-O]\d+|\d+[A-O]|-[A-Z?]*)(?:\s|$)/.test(
          events[index],
        )
      )
        continue;
      await page.selectOption("#history-position", String(index));
      const bag = parseInt(await page.locator("#bag-count").textContent());
      const mode = bag === 0 ? "endgame" : bag <= 4 ? "peg" : "sim";
      const before = await page.locator("#board").innerHTML();
      await page.locator(`[data-mode=${mode}]`).click();
      await page.waitForFunction(
        () => !document.body.classList.contains("busy"),
        null,
        { timeout: 45000 },
      );
      const row = {
        index,
        bag,
        mode,
        played: events[index],
        error: await page.locator("#error").textContent(),
        status: await page.locator("#status").textContent(),
        meta: await page.locator("#analysis-meta").textContent(),
        rows: await page.locator("#results tbody tr").allTextContents(),
        commands: await page.evaluate(() => window.qaCommands.at(-1)),
      };
      if (row.error)
        report.issues.push({
          type: "analysis-error",
          index,
          mode,
          error: row.error,
        });
      if (!row.meta.includes("Played #"))
        report.issues.push({ type: "played-move-missing", index, mode });
      if (!row.rows.length)
        report.issues.push({ type: "no-results", index, mode });
      if (before !== (await page.locator("#board").innerHTML()))
        report.issues.push({ type: "board-mutated-by-analysis", index });
      if (!savedPhases.has(mode) && row.rows.length) {
        savedPhases.add(mode);
        await page
          .locator("#results tbody tr")
          .first()
          .locator("button")
          .click();
        await page.screenshot({
          path: resolve(output, `game-${number}-${mode}.png`),
          fullPage: true,
        });
        if (mode === "endgame") {
          const pv = page.locator(".pv-line button");
          row.pvSteps = await pv.count();
          if (row.pvSteps) await pv.last().click();
        }
        if (mode === "sim") {
          row.beforeResume = await page
            .locator("#results tbody tr")
            .allTextContents();
          await page.locator("#continue-sim").click();
          await page.waitForFunction(
            () => !document.body.classList.contains("busy"),
            null,
            { timeout: 45000 },
          );
          row.afterResume = await page
            .locator("#results tbody tr")
            .allTextContents();
        }
        const table = await page.locator("#results").textContent();
        await page.selectOption("#history-position", String(index + 1));
        await page.selectOption("#history-position", String(index));
        if (table !== (await page.locator("#results").textContent()))
          report.issues.push({ type: "cache-restore", index, mode });
        row.restoredActive = await page
          .locator("[data-mode].active")
          .getAttribute("data-mode");
        if (row.restoredActive !== mode)
          report.issues.push({
            type: "active-mode",
            index,
            mode,
            actual: row.restoredActive,
          });
      }
      report.positions.push(row);
      log({
        game: number,
        index,
        mode,
        rows: row.rows.length,
        error: row.error,
      });
    }
    // Return from a late solver to an earlier cached simulation.
    await page.selectOption("#history-position", "0");
    const active = await page
      .locator("[data-mode].active")
      .getAttribute("data-mode");
    if (active !== "sim")
      report.issues.push({
        type: "active-mode",
        index: 0,
        expected: "sim",
        actual: active,
      });
    if (number === 1) {
      await page.locator("#review-game").click();
      await page.waitForFunction(
        () =>
          document.querySelector("#review-progress").textContent ===
          "Review complete",
        null,
        { timeout: 180000 },
      );
      report.wholeGame = {
        error: await page.locator("#error").textContent(),
        progress: await page.locator("#review-progress").textContent(),
        runs: await page.evaluate(() => window.qaCommands.length),
      };
    }
  } catch (e) {
    report.failure = e.message;
    await page
      .screenshot({
        path: resolve(output, `game-${number}-failure.png`),
        fullPage: true,
      })
      .catch(() => {});
  }
  reports.push(report);
  await writeFile(
    resolve(output, `game-${number}.json`),
    JSON.stringify(report, null, 2),
  );
  await writeFile(
    resolve(output, "summary.json"),
    JSON.stringify(reports, null, 2),
  );
  log({
    game: number,
    complete: !report.failure,
    issues: report.issues,
    failure: report.failure,
  });
  await page.close();
}
await browser.close();

if (reports.some((report) => report.failure || report.issues.length))
  process.exitCode = 1;
