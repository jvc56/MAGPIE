import { test, expect } from "@playwright/test";
async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await page.locator("#position-editor > summary").click();
}
async function setup(
  page,
  kind = "play",
  first = "0",
  time = "0",
  overtime = "0",
) {
  await page.locator("#new-game").click();
  await page.locator(`[data-game-kind="${kind}"]`).click();
  await page.locator(".game-options > summary").click();
  await page.selectOption("#first-player", first);
  await page.selectOption("#game-time", time);
  await page.selectOption("#overtime", overtime);
  await page.selectOption("#bot-analysis", "kibitz");
  if (kind === "watch") await page.locator("#watch-delay").fill("0");
  await page.locator("#new-game-form button.primary").click();
  await expect(page.locator("#play-session")).toBeVisible();
}
test("only the bag-appropriate search is visible, including while busy", async ({
  page,
}) => {
  await ready(page);
  for (const [sample, mode] of [
    ["opening", "sim"],
    ["peg", "peg"],
    ["endgame", "endgame"],
  ]) {
    await page.selectOption("#sample", sample);
    await expect(page.locator("[data-mode]:visible")).toHaveCount(2);
    await expect(page.locator(`[data-mode=${mode}]`)).toBeVisible();
  }
});
test("native play deals racks, keeps the human rack visible, and computer commits a legal reply", async ({
  page,
}) => {
  await ready(page);
  await setup(page);
  await expect(page.locator("#rack-tiles .rack-tile")).toHaveCount(7);
  const rack = await page.locator("#rack-tiles").textContent();
  await page.locator("#pass-move").click();
  await expect(page.locator("#play-move")).toBeEnabled();
  expect(await page.locator("#error").textContent()).toBe("");
  await expect(page.locator("#history-position option")).toHaveCount(3, {
    timeout: 30000,
  });
  await expect(page.locator("#rack-tiles")).toHaveText(rack);
  await expect(page.locator(".square.played").first()).toBeVisible();
  await expect(page.locator("#error")).toBeHidden();
  await page.locator("#pause-game").click();
  await expect(page.locator("#session-result")).toHaveText("Paused");
  await expect(page.locator("#play-move")).toBeDisabled();
  await page.screenshot({
    path: test.info().outputPath("play-computer-desktop.png"),
    fullPage: true,
  });
});
test("computer can start as player two and watching can run a complete game", async ({
  page,
}) => {
  test.setTimeout(180000);
  await ready(page);
  await setup(page, "watch", "1");
  await page.waitForFunction(
    () =>
      document
        .querySelector("#session-result")
        .textContent.includes("Game over") ||
      document.querySelector("#error").textContent,
    { timeout: 150000 },
  );
  expect(await page.locator("#error").textContent()).toBe("");
  await expect(page.locator("#session-result")).toContainText("Game over");
  await expect(page.locator("#move-entry")).toBeHidden();
  await expect(page.locator("#move-feedback")).toBeEmpty();
  await expect(page.locator("#error")).toBeHidden();
  expect(
    await page.locator("#history-position option").count(),
  ).toBeGreaterThan(10);
  await page.locator("#history-first").click();
  await expect(page.locator("#opponent-rack")).toHaveValue("");
  await page.locator("#hide-spoilers").check();
  await expect(
    page.locator("#history-table tbody button").first(),
  ).toHaveText("•••");
  await page.locator("#history-next").click();
  await expect(
    page.locator("#history-table tbody button").first(),
  ).not.toHaveText("•••");
  await page.locator("#hide-spoilers").uncheck();
  await page.screenshot({
    path: test.info().outputPath("game-review-desktop.png"),
    fullPage: true,
  });
});

test("live native transactions preserve racks and record withdrawn phonies", async ({
  page,
}) => {
  await ready(page);
  const outcome = await page.evaluate(async () => {
    const { EngineClient } = await import("/wasmentry/engine-client.mjs");
    const engine = new EngineClient();
    await engine.prepare("CSW24");
    const text =
      "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Alice\n#player2 p2 Magpie\n";
    const cgp = Array(15).fill("15").join("/") + " AEINRST/BCDGLOU 0/0 0";
    const start = await engine.gameAction({
      text,
      lexicon: "CSW24",
      index: 0,
      action: 4,
      cgp,
      seed: 12,
    });
    const request = {
      text: start.game.gcg,
      lexicon: "CSW24",
      index: 0,
      action: 5,
      cgp,
      rack: "AEINRST",
      move: "8H TSRAIN",
      seed: 13,
    };
    let rejected = "";
    try {
      await engine.gameAction({ ...request, challenge: "void" });
    } catch (error) {
      rejected = error.message;
    }
    const challenged = await engine.gameAction({
      ...request,
      challenge: "single",
    });
    const reimported = await engine.importGCG(challenged.game.gcg, "CSW24");
    engine.worker.terminate();
    return { rejected, game: challenged.game, imported: reimported.game };
  });
  expect(outcome.rejected).not.toBe("");
  expect(outcome.game.positions).toHaveLength(3);
  expect(outcome.game.positions.at(-1).cgp).toContain("BCDGLOU/AEINRST");
  expect(outcome.imported.positions).toHaveLength(3);
});

test("review keeps analysis per turn, continues sims, and branches with undo", async ({
  page,
}) => {
  await ready(page);
  await page
    .locator("#gcg-file")
    .setInputFiles("tests/fixtures/standard.gcg");
  await expect(page.locator("#game-history")).toBeVisible();
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("1");
  await page.locator("#candidates").fill("5");
  await page.locator("#back-to-game").click();
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete", {
    timeout: 30000,
  });
  await expect(page.locator("#results thead")).toContainText("Ply 1");
  const totalIterations = async () => {
    const meta = await page.locator("#analysis-meta").textContent();
    return Number(meta.match(/([\d,]+) total iterations/)[1].replaceAll(",", ""));
  };
  const batchIterations = async () => Number(
    [...(await page.locator("#raw").textContent()).matchAll(/Iters:\s+(\d+)/g)].at(-1)[1],
  );
  await expect(page.locator("[data-mode=sim]")).toHaveText("Continue sim");
  await expect(page.getByRole("button", { name: "Continue sim", exact: true })).toHaveCount(1);
  const firstTotal = await totalIterations();
  expect(firstTotal).toBe(await batchIterations());
  await expect(page.locator("#results thead")).toContainText("Iters");
  const first = await page.locator("#results tbody tr").first().textContent();
  await page.locator("#history-next").click();
  await page.locator("[data-mode=kibitz]").click();
  await expect(page.locator("#status")).toHaveText("Complete");
  await page.locator("#history-previous").click();
  await expect(page.locator("[data-mode=sim]")).toHaveClass(/active/);
  await expect(page.locator("#results tbody tr").first()).toHaveText(first);
  const before = await page
    .locator("#results tbody tr")
    .first()
    .locator("td")
    .nth(6)
    .textContent();
  const visibleRows = await page.locator("#results tbody tr").count();
  await page.locator("#results tbody button").first().click();
  const preview = await page.locator("#selection").textContent();
  await page.evaluate(() => {
    document.querySelector("[data-mode=sim]").addEventListener("click", () => {
      window.continueSnapshot = {
        rows: document.querySelectorAll("#results tbody tr").length,
        selection: document.getElementById("selection").textContent,
        empty: !document.getElementById("empty").hidden,
      };
    }, { once: true });
  });
  await page.locator("[data-mode=sim]").click();
  expect(await page.evaluate(() => window.continueSnapshot)).toEqual({
    rows: visibleRows, selection: preview, empty: false,
  });

  await expect(page.locator("#status")).toHaveText("Complete", {
    timeout: 30000,
  });
  const after = await page
    .locator("#results tbody tr")
    .first()
    .locator("td")
    .nth(6)
    .textContent();
  expect(Number(after)).toBeGreaterThan(Number(before));
  const secondTotal = await totalIterations();
  expect(secondTotal).toBe(firstTotal + await batchIterations());
  await page.locator("#history-next").click();
  await page.locator("#history-previous").click();
  expect(await totalIterations()).toBe(secondTotal);
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  expect(await totalIterations()).toBe(secondTotal + await batchIterations());
  const thirdTotal = await totalIterations();
  await page.evaluate(() => {
    window.resumeTotals = [];
    window.resumeObserver = new MutationObserver(() => {
      const text = document.getElementById("analysis-meta").textContent;
      const match = text.match(/([\d,]+) total iterations/);
      window.resumeTotals.push(match ? Number(match[1].replaceAll(",", "")) : 0);
    });
    window.resumeObserver.observe(document.getElementById("analysis-meta"), { childList: true, subtree: true });
  });
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  expect(await totalIterations()).toBe(thirdTotal + await batchIterations());
  const liveTotals = await page.evaluate(() => {
    window.resumeObserver.disconnect();
    return window.resumeTotals;
  });
  expect(liveTotals.length).toBeGreaterThan(0);
  expect(Math.min(...liveTotals)).toBeGreaterThanOrEqual(thirdTotal);
  await page.locator("#fresh-analysis").click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  expect(await totalIterations()).toBe(await batchIterations());

  const count = await page.locator("#history-position option").count();
  await page.locator("#play-from").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#play-session")).toBeVisible();
  await expect(page.locator("#history-position option")).toHaveCount(1);
  await expect(page.locator("#undo-game")).toBeDisabled();
  await page.locator("#finish-game").click();
  await page.locator("#confirm-yes").click();
  await page.locator("#undo-game").click();
  await expect(page.locator("#history-position option")).toHaveCount(count);
  await expect(page.locator("#play-session")).toBeHidden();
});

test("clock pauses, forfeits on time, and saves a valid GCG", async ({
  page,
}) => {
  await page.clock.install();
  await ready(page);
  await setup(page, "play", "0", "300", "0");
  await page.locator("#pause-game").click();
  const paused = await page.locator("#clock-one").textContent();
  await page.clock.fastForward(60000);
  await expect(page.locator("#clock-one")).toHaveText(paused);
  await page.locator("#pause-game").click();
  await page.clock.fastForward(301000);
  await expect(page.locator("#session-result")).toContainText("lost on time");
  await expect(page.locator("#move-entry")).toBeHidden();
  const download = page.waitForEvent("download");
  await page.locator("#game-menu > summary").click();
  await page.locator("#save-gcg").click();
  const saved = await download;
  await page.locator("#gcg-file").setInputFiles(await saved.path());
  await expect(page.locator("#error")).toBeHidden();
});

test("overtime penalties agree in the displayed score and saved GCG", async ({
  page,
}) => {
  await page.clock.install();
  await ready(page);
  await setup(page, "play", "0", "300", "60");
  await page.locator("#pass-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(3, {
    timeout: 30000,
  });
  await page.clock.fastForward(301000);
  await page.locator("#finish-game").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#session-result")).toContainText(
    "Time penalties 10 / 0",
  );
  await expect(page.locator("#score")).toHaveValue("-10");
  const download = page.waitForEvent("download");
  await page.locator("#game-menu > summary").click();
  await page.locator("#save-gcg").click();
  const saved = await download;
  await page.locator("#gcg-file").setInputFiles(await saved.path());
  await expect(page.locator("#status")).toContainText("Loaded", {
    timeout: 30000,
  });
  await expect(page.locator("#error")).toBeHidden();
  await page.locator("#history-last").click();
  await expect(page.locator("#score")).toHaveValue("-10");
});

test("postmortem sim includes the played move outside the top candidates", async ({
  page,
}) => {
  await ready(page);
  const gcg =
    "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Human\n#player2 p2 Computer\n>p1: AEINRST - +0 0\n";
  await page.locator("#gcg-file").setInputFiles({
    name: "pass.gcg",
    mimeType: "text/plain",
    buffer: Buffer.from(gcg),
  });
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#candidates").fill("2");
  await page.locator("#seconds").fill("1");
  await page.locator("#back-to-game").click();
  await page.locator("[data-mode=sim]").click();
  await page.waitForFunction(() =>
    ["Complete", "Analysis could not run"].includes(
      document.querySelector("#status").textContent,
    ),
  );
  expect(await page.locator("#error").textContent()).toBe("");
  await expect(page.locator("#status")).toHaveText("Complete", {
    timeout: 30000,
  });
  await expect(page.locator("#error")).toBeHidden();
  await expect(page.locator("#results tbody")).toContainText("pass");
  await expect(page.locator("#analysis-meta")).toContainText("Played #");
  await page.locator("[data-mode=sim]").click();
  await page.waitForFunction(() =>
    ["Complete", "Analysis could not run"].includes(
      document.querySelector("#status").textContent,
    ),
  );
  expect(await page.locator("#error").textContent()).toBe("");
  await expect(page.locator("#status")).toHaveText("Complete", {
    timeout: 30000,
  });
  await expect(page.locator("#results tbody")).toContainText("pass");
  // A full 100-candidate search still needs room for the extra played pass.
  await page.locator("#open-analysis-settings").click();
  await page.locator("#candidates").fill("100");
  await page.locator("#back-to-game").click();
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete");
  await expect(page.locator("#results tbody")).toContainText("pass");
  await expect(page.locator("#analysis-meta")).toContainText("Played #");
});

test("whole-game review reports failed analysis instead of silent success", async ({
  page,
}) => {
  await ready(page);
  const gcg =
    "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Human\n#player2 p2 Computer\n>p1: AEINRST - +0 0\n";
  await page.locator("#gcg-file").setInputFiles({
    name: "pass.gcg",
    mimeType: "text/plain",
    buffer: Buffer.from(gcg),
  });
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#candidates").fill("1");
  await page.locator("#back-to-game").click();
  await page.locator("#review-game").click();
  await expect(page.locator("#review-progress")).toHaveText(
    "Review complete · 1 failed",
  );
  await expect(page.locator("#stop-review")).toBeHidden();
  await expect(page.locator("#history-position")).toHaveValue("0");
});

test("PEG and endgame retain played moves below their normal result cutoff", async ({
  page,
}) => {
  await ready(page);
  await page
    .locator("#gcg-file")
    .setInputFiles("tests/fixtures/review-candidates.gcg");
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("1");
  await page.locator("#back-to-game").click();
  for (const [index, mode] of [
    [21, "peg"],
    [22, "endgame"],
    [24, "endgame"],
  ]) {
    await page.selectOption("#history-position", String(index));
    await page.locator(`[data-mode=${mode}]`).click();
    await page.waitForFunction(() =>
      ["Complete", "Analysis could not run"].includes(
        document.querySelector("#status").textContent,
      ),
    );
    expect(await page.locator("#error").textContent()).toBe("");
    await expect(page.locator("#status")).toHaveText("Complete", {
      timeout: 30000,
    });
    await expect(page.locator("#error")).toBeHidden();
    await expect(page.locator("#analysis-meta")).toContainText("Played #");
    if (mode === "endgame")
      await expect(page.locator("#results tbody tr")).toHaveCount(6);
  }
});

test("review adds real played placements and exchanges absent from generated sim candidates", async ({
  page,
}) => {
  test.setTimeout(60000);
  await page.addInitScript(() => {
    window.generated = [];
    window.reviewCommands = [];
    const send = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function (message, ...rest) {
      if (!this.observed) {
        this.observed = true;
        this.addEventListener("message", ({ data }) => {
          if (data.type === "output" && data.command === "generate")
            window.generated.push(data.text);
        });
      }
      if (message.type === "run")
        window.reviewCommands = message.data.commands;
      return send.call(this, message, ...rest);
    };
  });
  await ready(page);
  await page
    .locator("#gcg-file")
    .setInputFiles("tests/fixtures/review-candidates.gcg");
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("1");
  await page.locator("#candidates").fill("10");
  await page.locator("#back-to-game").click();
  for (const [index, played] of [
    [4, "ex AEE"],
    [15, "6K W..K"],
    [17, "B1 PYET"],
  ]) {
    await page.selectOption("#history-position", String(index));
    if (index === 15) {
      await page.locator("#rack").fill("WUTPKED");
      await page.locator("#rack").blur();
    }
    await page.locator("[data-mode=sim]").click();
    await expect(page.locator("#status")).toHaveText("Complete");
    const baseline = await page.evaluate(async (played) => {
      const { parseResults, previewMove, parseCGP } = await import(
        "/wasmentry/analysis-model.mjs"
      );
      const { normalizeMove } = await import("/wasmentry/move-entry.mjs");
      const position = parseCGP(
        window.reviewCommands.find((c) => c.startsWith("cgp ")).slice(4),
      );
      const identity = (move) => {
        const tiles = previewMove(position, move);
        return tiles.length
          ? JSON.stringify(
              tiles.sort((a, b) => a.row - b.row || a.col - b.col),
            )
          : normalizeMove(move);
      };
      const rows = parseResults(window.generated.at(-1), "kibitz");
      return {
        count: rows.length,
        containsPlayed: rows.some(
          (r) => identity(r.move) === identity(played),
        ),
        added: window.reviewCommands.includes(`addmoves ${played}`),
      };
    }, played);
    expect(baseline).toEqual({
      count: 10,
      containsPlayed: false,
      added: true,
    });
    await expect(page.locator("#analysis-meta")).toContainText("Played #");
    await page.locator("[data-mode=sim]").click();
    await expect(page.locator("#status")).toHaveText("Complete");
    await expect(page.locator("#analysis-meta")).toContainText("Played #");
  }
});


test("sim supports eight threads without resetting accumulated results", async ({ page }) => {
  await page.addInitScript(() => Object.defineProperty(navigator, "hardwareConcurrency", { get: () => 8 }));
  await ready(page);
  await page.locator("#gcg-file").setInputFiles("tests/fixtures/standard.gcg");
  await expect(page.locator("#game-history")).toBeVisible();
  await page.locator("#open-analysis-settings").click();
  await expect(page.locator("#threads")).toHaveAttribute("max", "8");
  await page.locator("#seconds").fill("2");
  await page.locator("#threads").fill("8");
  await page.locator("#back-to-game").click();
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  await expect(page.locator("#error")).toBeHidden();
  await expect(page.locator("#settings-summary")).toContainText("8 threads");
  await expect(page.locator("#results tbody tr").first()).toBeVisible();
  const first = Number((await page.locator("#analysis-meta").textContent()).match(/([\d,]+) total iterations/)[1].replaceAll(",", ""));
  await page.locator("[data-mode=sim]").click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  const second = Number((await page.locator("#analysis-meta").textContent()).match(/([\d,]+) total iterations/)[1].replaceAll(",", ""));
  expect(second).toBeGreaterThan(first);
});
