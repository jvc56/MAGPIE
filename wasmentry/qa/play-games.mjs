import { chromium } from "playwright";
import { mkdir, writeFile, readFile, readdir } from "node:fs/promises";
import { resolve } from "node:path";
import { findPlay } from "./playfinder.mjs";
import { parseCGP, previewMove } from "../analysis-model.mjs";

const args = process.argv.slice(2);
const option = (key, fallback) =>
  args.includes(key) ? args[args.indexOf(key) + 1] : fallback;
const count = Number(option("--games", "10"));
const output = resolve(option("--output", "/private/tmp/magpie-blitz-qa"));
const url = option("--url", "http://localhost:8000/wasmentry/");
const start = Number(option("--start", "1"));
await mkdir(output, { recursive: true });
const browser = await chromium.launch();

const log = (event) =>
  console.log(JSON.stringify({ time: new Date().toISOString(), ...event }));
async function saveGame(page, path) {
  const download = page.waitForEvent("download");
  await page.locator("#save-gcg").click();
  await (await download).saveAs(path);
}
async function state(page) {
  return page.evaluate(() => {
    const get = (id) => document.getElementById(id);
    const board = Array.from({ length: 15 }, () => Array(15).fill(""));
    document
      .querySelectorAll(".square.played:not(.preview):not(.pending)")
      .forEach((cell) => {
        let letter = cell.querySelector(".tile-letter").textContent;
        if (cell.classList.contains("blank")) letter = letter.toLowerCase();
        board[+cell.dataset.row][+cell.dataset.col] = letter;
      });
    const encoded = board
      .map((row) => {
        let n = 0,
          s = "";
        for (const tile of row) {
          if (!tile) n++;
          else {
            if (n) s += n;
            n = 0;
            s += tile;
          }
        }
        return s + (n || "");
      })
      .join("/");
    const rack = [...get("rack-tiles").querySelectorAll("[data-letter]")]
      .map((t) => t.dataset.letter)
      .join("");
    return {
      cgp: `${encoded} ${rack}/ ${get("score").value}/${get("opponent-score").value} 0 -lex ${get("current-lexicon").textContent}`,
      result: get("session-result").textContent,
      error: get("error").textContent,
      ready: !get("move-notation").disabled,
      ended: get("pause-game").disabled,
      events: get("history-position").options.length - 1,
      clocks: [get("clock-one").textContent, get("clock-two").textContent],
      bag: get("bag-count")?.textContent,
      overflow: document.documentElement.scrollWidth > innerWidth,
    };
  });
}
for (let number = start; number < start + count; number++) {
  const mobile = number % 2 === 0;
  const context = await browser.newContext({
    viewport: mobile
      ? { width: 390, height: 844 }
      : { width: 1440, height: 1100 },
    hasTouch: mobile,
  });
  const page = await context.newPage();
  page.setDefaultTimeout(12000);
  const record = {
    number,
    mobile,
    first: number % 2 ? "0" : "1",
    timeControl: 180,
    computer: "auto",
    computerSeconds: 5,
    moves: [],
    issues: [],
    started: new Date().toISOString(),
  };
  page.on("pageerror", (error) =>
    record.issues.push({ type: "pageerror", message: error.message }),
  );
  try {
    await page.goto(url);
    await page.waitForFunction(
      () => document.querySelector("#status")?.textContent === "Engine ready",
      null,
      { timeout: 45000 },
    );
    await page.locator("#new-game").click();
    await page.selectOption("#game-kind", "play");
    await page.locator("#player-one-name").fill("CLI Magpie");
    await page.locator("#player-two-name").fill("Browser Magpie");
    await page.selectOption("#human-side", "0");
    await page.selectOption("#first-player", record.first);
    await page.selectOption("#game-time", "180");
    await page.selectOption("#overtime", "0");
    await page.selectOption("#bot-analysis", "auto");
    await page.locator("#new-game-form button.primary").click();
    await page.locator("#play-session").waitFor({ state: "visible" });
    log({ game: number, event: "started", mobile });
    const deadline = Date.now() + 440000;
    let previousEvents = -1;
    while (Date.now() < deadline) {
      await page.waitForFunction(
        () =>
          document.querySelector("#pause-game").disabled ||
          !document.querySelector("#move-notation").disabled ||
          document.querySelector("#error").textContent,
        null,
        { timeout: 45000 },
      );
      const before = await state(page);
      if (before.error) throw new Error(before.error);
      if (before.ended) {
        record.result = before.result;
        record.clocks = before.clocks;
        break;
      }
      if (before.events === previousEvents)
        throw new Error("Turn did not advance after submitted move");
      previousEvents = before.events;
      if (before.overflow)
        record.issues.push({
          type: "layout",
          message: "Horizontal page overflow",
          events: before.events,
        });
      const found = findPlay(before.cgp);
      const turn = record.moves.length;
      let move = found.move;
      if (turn === 2 && [3, 7].includes(number))
        move = `ex ${parseCGP(before.cgp).racks[0].slice(0, 3)}`;
      const method =
        move.startsWith("ex ") || move === "pass"
          ? "notation"
          : ["notation", "tap", "keyboard"][turn % 3];
      if (turn === 4) {
        await page.locator("#pause-game").click();
        const paused = await state(page);
        await page.locator("#open-settings").click();
        await page.selectOption("#theme", number % 2 ? "light" : "dark");
        await page.locator("#back-to-game").click();
        const after = await state(page);
        if (paused.clocks.join() !== after.clocks.join())
          record.issues.push({
            type: "clock",
            message: "Clock changed while paused",
          });
        await page.locator("#pause-game").click();
      }
      if (method === "notation") {
        await page.locator("#move-notation").fill(move);
        await page.locator("#move-notation").press("Enter");
      } else {
        const tiles = previewMove(parseCGP(before.cgp), move);
        if (!tiles.length) throw new Error(`No placements for ${move}`);
        for (const tile of tiles) {
          const square = page.locator(
            `[data-row="${tile.row}"][data-col="${tile.col}"]`,
          );
          if (method === "tap") {
            const blank = tile.letter === tile.letter.toLowerCase();
            await page
              .locator(
                `#rack-tiles [data-letter="${blank ? "?" : tile.letter}"]`,
              )
              .first()
              .click();
            await square.click();
            if (blank)
              await page
                .getByRole("button", {
                  name: `Blank as ${tile.letter.toUpperCase()}`,
                  exact: true,
                })
                .click();
          } else {
            await square.click();
            await page.keyboard.press(
              tile.letter === tile.letter.toLowerCase()
                ? `Shift+${tile.letter}`
                : tile.letter.toLowerCase(),
            );
          }
        }
        const feedback = await page.locator("#error").textContent();
        if (feedback) throw new Error(feedback);
        await page.locator("#play-move").click();
      }
      record.moves.push({
        move,
        method,
        cgp: found.cgp,
        nativeMs: found.milliseconds,
        clocks: before.clocks,
        events: before.events,
      });
      log({
        game: number,
        turn: turn + 1,
        move,
        method,
        clocks: before.clocks,
      });
      await page.waitForFunction(
        (events) =>
          document.querySelector("#history-position").options.length - 1 >
            events ||
          document.querySelector("#error").textContent ||
          document.querySelector("#pause-game").disabled,
        before.events,
        { timeout: 20000 },
      );
    }
    if (!record.result) throw new Error("Game exceeded wall-clock watchdog");
    await saveGame(page, resolve(output, `game-${number}.gcg`));
    await page.screenshot({
      path: resolve(output, `game-${number}-final.png`),
      fullPage: true,
    });
    await page.locator("#history-first").click();
    await page.locator("#history-last").click();
    const scores =
      (await page.locator("#score").inputValue()) +
      "/" +
      (await page.locator("#opponent-score").inputValue());
    await page
      .locator("#gcg-file")
      .setInputFiles(resolve(output, `game-${number}.gcg`));
    await page.waitForFunction(
      () =>
        document.querySelector("#status").textContent.startsWith("Loaded") ||
        document.querySelector("#error").textContent,
    );
    if (await page.locator("#error").textContent())
      throw new Error("Saved GCG failed to reload");
    await page.locator("#history-last").click();
    const restored =
      (await page.locator("#score").inputValue()) +
      "/" +
      (await page.locator("#opponent-score").inputValue());
    if (scores !== restored)
      record.issues.push({ type: "roundtrip", scores, restored });
    record.completed = true;
  } catch (error) {
    record.failure = error.message;
    record.state = await state(page).catch(() => null);
    await page
      .screenshot({
        path: resolve(output, `game-${number}-failure.png`),
        fullPage: true,
      })
      .catch(() => {});
    if (
      await page
        .locator("#pause-game")
        .isEnabled()
        .catch(() => false)
    )
      await page
        .locator("#pause-game")
        .click()
        .catch(() => {});
    await saveGame(page, resolve(output, `game-${number}-partial.gcg`)).catch(
      () => {},
    );
  }
  record.finished = new Date().toISOString();
  await writeFile(
    resolve(output, `game-${number}.json`),
    JSON.stringify(record, null, 2),
  );
  const summaries = await Promise.all(
    (await readdir(output))
      .filter((name) => /^game-\d+\.json$/.test(name))
      .map(async (name) =>
        JSON.parse(await readFile(resolve(output, name), "utf8")),
      ),
  );
  summaries.sort((a, b) => a.number - b.number);
  await writeFile(
    resolve(output, "summary.json"),
    JSON.stringify(summaries, null, 2),
  );
  log({
    game: number,
    event: record.completed ? "completed" : "failed",
    result: record.result,
    failure: record.failure,
    issues: record.issues,
  });
  await context.close();
  if (!record.completed) break;
}
await browser.close();
