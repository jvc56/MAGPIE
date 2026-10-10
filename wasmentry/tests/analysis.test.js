import { readFile } from "node:fs/promises";
import { test, expect } from "@playwright/test";

async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("2");
  await page.locator("#threads").fill("4");
  await page.locator("#back-to-game").click();
  await page.locator("#position-editor > summary").click();
}
async function analyze(page, mode) {
  await page.locator(`[data-mode="${mode}"]`).click();
  await expect(page.locator("#stop")).toBeDisabled({ timeout: 60000 });
  expect(await page.locator("#error").textContent()).toBe("");
  await expect(page.locator("#status")).toHaveText("Complete");
  const raw = await page.locator("#raw").textContent();
  await test
    .info()
    .attach(`${mode}-output`, { body: raw, contentType: "text/plain" });
  expect(raw).not.toBe("");
  await expect(page.locator("#results tbody tr").first()).toBeVisible();
}

test("all four modes use the real WASM engine and clear stale results", async ({
  page,
}) => {
  test.setTimeout(150000);
  await ready(page);
  await analyze(page, "kibitz");
  await expect(page.locator("#results tbody tr")).toHaveCount(100);
  await page.locator("#results tbody button").first().click();
  await expect(page.locator(".square.preview").first()).toBeVisible();
  await page.selectOption("#sample", "midgame");
  await expect(page.locator("#results tbody tr")).toHaveCount(0);
  await expect(page.locator(".square.preview")).toHaveCount(0);
  await analyze(page, "sim");
  await expect(page.locator("#results thead")).toContainText("Ply 1");
  await page.screenshot({
    path: test.info().outputPath("analysis-sim.png"),
    fullPage: true,
  });
  await expect(page.locator("#results")).toContainText("14F ZI(N)E");
  await page.selectOption("#sample", "peg");
  await analyze(page, "peg");
  await expect(page.locator("#results thead")).toContainText("W / T / L");
  await page.screenshot({
    path: test.info().outputPath("analysis-peg.png"),
    fullPage: true,
  });
  await page.selectOption("#sample", "endgame");
  await analyze(page, "endgame");
  await expect(page.locator("#results tbody tr")).toHaveCount(5);
  await page.locator("#results tbody button").first().click();
  await expect(page.locator(".pv-line button").first()).toBeVisible();
  await page.locator(".pv-line button").last().click();
  await page.screenshot({
    path: test.info().outputPath("analysis-endgame.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 375, height: 844 });
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBe(375);
  await page.screenshot({
    path: test.info().outputPath("analysis-endgame-mobile.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 1280, height: 720 });
  await page.screenshot({
    path: "test-results/analysis-desktop.png",
    fullPage: true,
  });
});

test("stop permits another analysis, invalid CGP preserves position, edits undo", async ({
  page,
}) => {
  test.setTimeout(90000);
  await ready(page);
  await page.locator("#game-menu > summary").click();
  await page.locator("#import").click();
  await page.locator("#cgp").fill("not a position");
  await page.getByRole("button", { name: "Load position" }).click();
  await expect(page.locator("#import-error")).not.toHaveText("");
  await page.locator("#close-import").click();
  await expect(page.locator("#rack")).toHaveValue("AEINRST");
  await page.locator("#edit").check();
  await page.locator('[data-row="7"][data-col="7"]').click();
  await page.keyboard.type("cat");
  await expect(page.locator('[data-row="7"][data-col="7"]')).toHaveAttribute(
    "aria-label",
    "H8 C · Player 1",
  );
  await page.locator("#undo").click();
  await expect(page.locator('[data-row="7"][data-col="9"]')).not.toHaveClass(
    /played/,
  );
  await page.selectOption("#sample", "midgame");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("120");
  await page.locator("#back-to-game").click();
  await page.locator('[data-mode="sim"]').click();
  await expect(page.locator("#status")).toHaveText("Simulating…", {
    timeout: 30000,
  });
  await expect(page.locator("#results tbody tr").first()).toBeVisible({
    timeout: 20000,
  });
  await page.locator("#open-settings").click();
  await expect(page.locator("#back-to-game")).toBeEnabled();
  await expect(page.locator("#premium-labels")).toBeEnabled();
  await page.locator('[data-settings-section="analysis"]').click();
  await expect(page.locator("#seconds")).toBeDisabled();
  await expect(page.locator("#settings-busy")).toBeVisible();
  await page.keyboard.press("Escape");
  await expect(page.locator("#stop")).toBeEnabled();
  await page.locator("#stop").click();
  await expect(page.locator("#status")).toHaveText(
    "Stopped · partial results",
    { timeout: 10000 },
  );
  await analyze(page, "kibitz");
  await page.setViewportSize({ width: 390, height: 844 });
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBe(390);
  await page.screenshot({
    path: "test-results/analysis-mobile.png",
    fullPage: true,
  });
});

test("settings replaces the game, preserves results, and saves board preferences", async ({
  page,
}) => {
  await ready(page);
  await analyze(page, "kibitz");
  await page.locator("#results tbody button").first().click();
  const board = await page.locator("#board").innerHTML();
  const results = await page.locator("#results").innerHTML();
  await page.locator("#open-settings").click();
  await expect(page.locator("#game-view")).toBeHidden();
  await expect(page.locator("#settings-view")).toBeVisible();
  await expect(page.locator("#settings-heading")).toBeFocused();
  await expect(page.locator("#premium-labels")).not.toBeChecked();
  await page.locator('[data-settings-section="board"]').click();
  await page.locator("#premium-labels").check();
  await page.locator('[data-settings-section="analysis"]').click();
  await expect(page.locator("#analysis-settings")).toBeVisible();
  await page.locator("#seconds").fill("3");
  await page.locator("#settings-heading").focus();
  await page.keyboard.press("s");
  await expect(page.locator("#status")).toHaveText("Complete");
  await page.keyboard.press("Escape");
  await expect(page.locator("#game-view")).toBeVisible();
  await expect(page.locator("#open-settings")).toBeFocused();
  expect(await page.locator("#board").innerHTML()).toBe(board);
  expect(await page.locator("#results").innerHTML()).toBe(results);
  await expect(page.locator("#settings-summary")).toContainText("3s");
  await expect(page.locator("#board")).toHaveClass(/show-premium-labels/);
  await page.reload();
  await expect(page.locator("#status")).toHaveText("Engine ready");
  await expect(page.locator("#board")).toHaveClass(/show-premium-labels/);
  await page.locator("#open-settings").click();
  await expect(page.locator("#premium-labels")).toBeChecked();
  await page.setViewportSize({ width: 375, height: 844 });
  expect(
    await page.evaluate(() => document.documentElement.scrollWidth),
  ).toBe(375);
  await page.locator("#back-to-game").click();
  await expect(page.locator("#settings-view")).toBeHidden();
});

test("lexicon has its own settings section and clears analysis when changed", async ({
  page,
}) => {
  await ready(page);
  await analyze(page, "kibitz");
  const board = await page.locator("#board").innerHTML();
  const rack = await page.locator("#rack").inputValue();
  await page.locator("#open-lexicon-settings").click();
  await expect(page.locator("#lexicon-settings")).toBeVisible();
  await expect(page.locator("#analysis-settings")).toBeHidden();
  await page.selectOption("#lexicon", "NWL23");
  await page.locator("#back-to-game").click();
  await expect(page.locator("#current-lexicon")).toHaveText("NWL23");
  await expect(page.locator("#rack")).toHaveValue(rack);
  expect(await page.locator("#board").innerHTML()).toBe(board);
  await expect(page.locator("#results tbody tr")).toHaveCount(0);
  await analyze(page, "kibitz");
  await page.locator("#open-settings").click();
  await page.locator('[data-settings-section="lexicon"]').click();
  await expect(page.locator("#lexicon")).toHaveValue("NWL23");
});

test("worker rejects a bad command and can run a new batch", async ({
  page,
}) => {
  test.setTimeout(60000);
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  const result = await page.evaluate(async () => {
    const { EngineClient } = await import("/wasmentry/engine-client.mjs");
    const engine = new EngineClient();
    try {
      await engine.prepare("CSW24");
      let error = "";
      try {
        await engine.run(["not-a-magpie-command"]);
      } catch (failure) {
        error = failure.message;
      }
      const completed = await engine.run(["set -lex CSW24 -wmp false"]);
      return { error, completed };
    } finally {
      engine.worker.terminate();
    }
  });
  expect(result.error).not.toBe("");
  expect(result.completed.type).toBe("complete");
});

test("Qt blanks and player tile colors survive undo and recent recall", async ({
  page,
}) => {
  await ready(page);
  await page.locator("#edit").check();
  const first = page.locator('[data-row="7"][data-col="7"]');
  const second = page.locator('[data-row="8"][data-col="7"]');
  await first.click();
  await page.keyboard.type("cat");
  await expect(first).toHaveClass(/owner-0/);
  await page.selectOption("#tile-owner", "1");
  await second.click();
  await page.keyboard.press("Shift+D");
  await expect(second).toHaveClass(/blank.*owner-1/);
  await expect(second.locator("sup")).toHaveCount(0);
  await expect(second).toHaveCSS("font-style", "normal");
  await expect(second.locator(".tile-letter")).toHaveCSS(
    "border-top-style",
    "solid",
  );
  expect(
    await first.evaluate((el) => getComputedStyle(el).backgroundColor),
  ).not.toBe(
    await second.evaluate((el) => getComputedStyle(el).backgroundColor),
  );
  await first.click();
  await page.locator("#assign-owner").click();
  await expect(first).toHaveClass(/owner-1/);
  await page.locator("#undo").click();
  await expect(first).toHaveClass(/owner-0/);
  await page.selectOption("#on-turn-player", "1");
  await analyze(page, "kibitz");
  await page.reload();
  await page.locator("#position-editor > summary").click();
  await page.selectOption("#recent", "0");
  await expect(first).toHaveClass(/owner-0/);
  await expect(second).toHaveClass(/owner-1/);
  await expect(page.locator("#on-turn-player")).toHaveValue("1");
  await page.screenshot({
    path: "test-results/analysis-player-colors.png",
    fullPage: true,
  });
  await page.selectOption("#sample", "midgame");
  await expect(page.locator(".square.owner-0,.square.owner-1")).toHaveCount(
    0,
  );
  await expect(page.locator(".square.owner-unknown").first()).toBeVisible();
});

test("engine bootstrap isolates an ordinary static host and reloads only once", async ({
  page,
}, testInfo) => {
  let navigations = 0;
  page.on("framenavigated", (frame) => {
    if (frame === page.mainFrame()) navigations++;
  });
  const response = await page.goto("/wasmentry/");
  if (testInfo.project.name === "static-host") {
    expect(response.headers()["cross-origin-opener-policy"]).toBeUndefined();
    expect(
      response.headers()["cross-origin-embedder-policy"],
    ).toBeUndefined();
  }
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  expect(await page.evaluate(() => crossOriginIsolated)).toBe(true);
  expect(navigations).toBe(testInfo.project.name === "static-host" ? 2 : 1);
  const beforeReload = navigations;
  await page.reload();
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  expect(navigations).toBe(beforeReload + 1);
});

const gameFixture = `#character-encoding UTF-8
#title Browser round trip
#lexicon CSW24
#player1 Ada Ada Example
#player2 Bo Bo Example
>Ada: AEEGTTV 8B GAVETTE +80 80
>Ada: AEEGTTV -- -80 0
>Bo: DEIIKOR -KOI +0 0
#note A note with "quotes", <tags> and a backslash \\.
>Ada: AEEGTTV 8D VETTAGE +80 80
>Ada: AEEGTTV -- -80 0
>Bo: DDEIRRT -DR +0 0
>Ada: AEEGTTV -VG +0 0
>Bo: DEGIRRT -DG +0 0
>Ada: ?AEEOTT (?AEEOTT) -6 -6
>Bo: EIOQRRT (EIOQRRT) -16 -16
`;
async function loadGCG(page, text, name = "record.gcg") {
  await page.locator("#gcg-file").setInputFiles({
    name,
    mimeType: "text/plain",
    buffer: Buffer.from(text),
  });
  await expect(page.locator("#load-gcg")).toBeEnabled({ timeout: 60000 });
}
test("GCG native round trip, history, notes, errors and analysis isolation", async ({
  page,
}) => {
  test.setTimeout(120000);
  await ready(page);
  await expect(page.locator("#save-gcg")).toBeDisabled();
  await loadGCG(page, gameFixture);
  await expect(page.locator("#error")).toBeHidden();
  await expect(page.locator("#game-players")).toHaveText(
    "Ada Example vs Bo Example",
  );
  await expect(page.locator("#history-position option")).toHaveCount(11);
  await page.selectOption("#history-position", "1");
  await expect(page.locator(".square.played.owner-0")).toHaveCount(7);
  await page.locator("#history-next").click();
  await expect(page.locator(".square.played")).toHaveCount(0);
  await page.locator("#history-next").click();
  await expect(page.locator("#history-note")).toContainText("<tags>");
  await analyze(page, "kibitz");
  await page.locator("#score").fill("123");
  await page.locator("#score").dispatchEvent("change");
  await expect(page.locator("#history-edited")).toBeVisible();
  const downloadEvent = page.waitForEvent("download");
  await page.locator("#game-menu > summary").click();
  await page.locator("#save-gcg").click();
  const download = await downloadEvent;
  expect(download.suggestedFilename()).toBe("record.gcg");
  const saved = await readFile(await download.path(), "utf8");
  expect(saved).toContain("#title Browser round trip");
  expect(saved).toContain('#note A note with "quotes", <tags>');
  expect(saved.match(/^>/gm)).toHaveLength(10);
  await loadGCG(page, saved, "roundtrip.gcg");
  await expect(page.locator("#error")).toBeHidden();
  await page.selectOption("#history-position", "10");
  const scores = [
    await page.locator("#score").inputValue(),
    await page.locator("#opponent-score").inputValue(),
  ].sort();
  expect(scores).toEqual(["-16", "-6"]);
  await loadGCG(page, "invalid gcg");
  await expect(page.locator("#error")).toBeVisible();
  await expect(page.locator("#game-players")).toHaveText(
    "Ada Example vs Bo Example",
  );
  await expect(page.locator("#history-position")).toHaveValue("10");
  await loadGCG(page, gameFixture.replace("CSW24", "CSW21"));
  await expect(page.locator("#error")).toContainText("CSW21");
  await expect(page.locator("#save-gcg")).toBeEnabled();
  await page.selectOption("#sample", "opening");
  await expect(page.locator("#game-history")).toBeHidden();
  await expect(page.locator("#save-gcg")).toBeDisabled();
});

test("GCG full game preserves blanks, both owners, final scores and UTF-8", async ({
  page,
}) => {
  await ready(page);
  const fixture = await readFile("tests/fixtures/standard.gcg", "utf8");
  await loadGCG(
    page,
    fixture.replace("#player1 HastyBot HastyBot", "#player1 HastyBot Césár"),
  );
  await expect(page.locator("#error")).toBeHidden();
  await expect(page.locator("#game-players")).toContainText("Césár");
  await page.selectOption("#history-position", "3");
  await expect(page.locator(".square.owner-1")).toHaveCount(2);
  await expect(page.locator(".square.owner-0")).toHaveCount(7);
  await expect(page.locator(".square.blank.owner-0")).toHaveCount(1);
  await analyze(page, "kibitz");
  const count = await page.locator("#history-position option").count();
  await page.selectOption("#history-position", String(count - 1));
  const before = [
    await page.locator("#score").inputValue(),
    await page.locator("#opponent-score").inputValue(),
  ];
  const downloadEvent = page.waitForEvent("download");
  await page.locator("#game-menu > summary").click();
  await page.locator("#save-gcg").click();
  const saved = await readFile(await (await downloadEvent).path(), "utf8");
  await loadGCG(page, saved);
  await page.selectOption("#history-position", String(count - 1));
  expect([
    await page.locator("#score").inputValue(),
    await page.locator("#opponent-score").inputValue(),
  ]).toEqual(before);
  await page.selectOption("#history-position", "15");
  await page.screenshot({
    path: test.info().outputPath("gcg-desktop.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 375, height: 844 });
  await page.screenshot({
    path: test.info().outputPath("gcg-mobile.png"),
    fullPage: true,
  });
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

async function newGame(page, rack = "AEINRST") {
  await page.locator("#new-game").click();
  await page.getByRole("button", { name: "Record a game", exact: true }).click();
  await page.locator("#new-game-form button.primary").click();
  await expect(page.locator("#status")).toHaveText(
    "Game ready · enter the first rack",
    { timeout: 60000 },
  );
  await page.locator("#rack").fill(rack);
  await page.locator("#rack").dispatchEvent("change");
}
async function playTyped(page, word, row = 7, col = 7) {
  await page.locator(`[data-row="${row}"][data-col="${col}"]`).click();
  await page.keyboard.type(word);
  await expect(page.locator("#move-feedback")).toContainText("points", {
    timeout: 10000,
  });
  await page.keyboard.press("Enter");
  await expect(page.locator("#status")).toHaveText("Move recorded", {
    timeout: 30000,
  });
}
test("game entry: typing, live rack, scoring, pass, exchange, notes and saved history", async ({
  page,
}) => {
  await ready(page);
  await newGame(page);
  await playTyped(page, "train");
  await expect(page.locator(".square.played.owner-0")).toHaveCount(5);
  await expect(page.locator("#opponent-score")).toHaveValue("12");
  await expect(page.locator("#history-position option")).toHaveCount(2);
  await page.locator("#rack").fill("ABCDEFS");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator("#pass-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.locator("#rack").fill("AEINRST");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator("#exchange-move").click();
  await expect(page.locator("#confirm-exchange")).toBeDisabled();
  await page.locator('#exchange-tiles [data-letter="A"]').click();
  await page.locator('#exchange-tiles [data-letter="E"]').click();
  await expect(page.locator("#confirm-exchange")).toHaveText("Exchange 2 tiles");
  await page.locator("#confirm-exchange").click();
  await expect(page.locator("#history-position option")).toHaveCount(4);
  await page.locator("#annotation summary").click();
  await page.locator("#event-note").fill('Recorded on web — "note"');
  await page.locator("#save-note").click();
  await expect(page.locator("#history-note")).toHaveText(
    'Recorded on web — "note"',
  );
  const downloaded = page.waitForEvent("download");
  await page.locator("#game-menu > summary").click();
  await page.locator("#save-gcg").click();
  const saved = await readFile(await (await downloaded).path(), "utf8");
  expect(saved).toContain("TRAIN");
  expect(saved).toContain("-AE");
  expect(saved).toContain('Recorded on web — "note"');
  await loadGCG(page, saved);
  await expect(page.locator("#error")).toBeHidden();
  await expect(page.locator("#history-position option")).toHaveCount(4);
});
test("move entry: drag, tap, blank, recall, direction, playthrough and validation", async ({
  page,
}) => {
  await ready(page);
  await newGame(page, "AENRST?");
  const tile = page.locator('#rack-tiles [data-letter="T"]');
  const square = page.locator('[data-row="7"][data-col="7"]');
  await tile.scrollIntoViewIfNeeded();
  const from = await tile.boundingBox(),
    to = await square.boundingBox();
  await page.mouse.move(from.x + from.width / 2, from.y + from.height / 2);
  await page.mouse.down();
  await page.mouse.move(to.x + to.width / 2, to.y + to.height / 2, {
    steps: 8,
  });
  await page.mouse.up();
  await expect(page.locator(".square.pending")).toHaveCount(1);
  await page.locator('#rack-tiles [data-letter="?"]').click();
  await page.locator('[data-row="7"][data-col="8"]').click();
  await page.getByRole("button", { name: "Blank as O", exact: true }).click();
  await expect(page.locator(".square.pending.blank")).toHaveCount(1);
  await page.locator("#play-move").click();
  await expect(page.locator("#status")).toHaveText("Move recorded");
  await page.locator("#rack").fill("AEINRST");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator('[data-row="7"][data-col="9"]').click();
  await page.keyboard.type("e");
  await expect(page.locator('.square.pending[data-row="7"][data-col="9"]')).toHaveCount(1);
  await page.keyboard.press("Backspace");
  await expect(page.locator(".square.pending")).toHaveCount(0);
  await page.keyboard.type("e");
  await page.keyboard.press("Enter");
  await expect(page.locator("#status")).toHaveText("Move recorded");
  await expect(page.locator(".square.played.owner-1")).toHaveCount(1);
  await page.locator("#rack").fill("AEINRST");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator('[data-row="0"][data-col="0"]').click();
  await page.keyboard.type("rat");
  await page.keyboard.press("Space");
  await expect(page.locator('.square.pending[data-col="0"]')).toHaveCount(3);
  await page.keyboard.press("Enter");
  await expect(page.locator("#error")).toBeVisible();
  await expect(page.locator(".square.pending")).toHaveCount(3);
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.locator("#recall-tiles").click();
  await expect(page.locator(".square.pending")).toHaveCount(0);
});
test("history replacement confirms, cancels and undoes without losing later moves", async ({
  page,
}) => {
  await ready(page);
  await newGame(page);
  await playTyped(page, "train");
  await page.locator("#rack").fill("ABCDEFS");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator("#pass-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.selectOption("#history-position", "1");
  await page.locator("#edit-history-move").click();
  await expect(page.locator(".square.pending")).toHaveCount(5);
  await page.locator("#recall-tiles").click();
  await page.locator('[data-row="7"][data-col="7"]').click();
  await page.keyboard.type("rain");
  await page.locator("#play-move").click();
  await expect(page.locator("#game-confirm")).toBeVisible();
  await page.locator("#confirm-no").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.locator("#play-move").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#history-position option")).toHaveCount(2);
  await expect(page.locator(".square.played")).toHaveCount(4);
  await page.locator("#undo-game").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.selectOption("#history-position", "1");
  await expect(page.locator(".square.played")).toHaveCount(5);
});

test("phony confirmation, challenge removal and candidate commit use native history", async ({
  page,
}) => {
  await ready(page);
  await newGame(page, "AEINRST");
  await page.locator('[data-row="7"][data-col="7"]').click();
  await page.keyboard.type("tsrain");
  await page.locator("#play-move").click();
  await expect(page.locator("#confirm-title")).toHaveText(
    "Word not in the lexicon",
  );
  await page.keyboard.press("Escape");
  await expect(page.locator("#game-confirm")).toBeHidden();
  await expect(page.locator("#history-position option")).toHaveCount(1);
  await page.locator("#play-move").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#history-position option")).toHaveCount(2);
  await page.locator("#annotation summary").click();
  await page.locator("#challenge-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await expect(page.locator(".square.played")).toHaveCount(0);
  await page.locator("#rack").fill("AEINRST");
  await page.locator("#rack").dispatchEvent("change");
  await analyze(page, "kibitz");
  await page.locator("#results tbody button").first().click();
  await page.getByRole("button", { name: "Play selected move" }).click();
  await expect(page.locator("#status")).toHaveText("Move recorded");
  await expect(page.locator(".square.played")).toHaveCount(7);
});
test("mobile taps and touch drag place tiles, and a pending tile can return to the rack", async ({
  page,
}) => {
  await page.setViewportSize({ width: 375, height: 844 });
  await ready(page);
  await newGame(page, "AEINRST");
  await page.locator('#rack-tiles [data-letter="A"]').click();
  await page.locator('[data-row="7"][data-col="7"]').click();
  await expect(page.locator(".square.pending")).toHaveCount(1);
  await expect(page.locator('#rack-tiles [data-letter="A"]')).toHaveCount(0);
  await page.locator("#recall-tiles").click();
  // Use real touch events through Chromium's device input, not synthetic DOM
  // handlers, to exercise pointer capture and touch-action behavior.
  const client = await page.context().newCDPSession(page);
  await page
    .locator('#rack-tiles [data-letter="A"]')
    .scrollIntoViewIfNeeded();
  const from = await page
    .locator('#rack-tiles [data-letter="A"]')
    .boundingBox();
  const to = await page
    .locator('[data-row="14"][data-col="7"]')
    .boundingBox();
  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [
      { x: from.x + from.width / 2, y: from.y + from.height / 2 },
    ],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [{ x: to.x + to.width / 2, y: to.y + to.height / 2 }],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  await expect(page.locator(".square.pending")).toHaveCount(1);
  const back = await page.locator("#rack-tiles").boundingBox();
  await client.send("Input.dispatchTouchEvent", {
    type: "touchStart",
    touchPoints: [{ x: to.x + to.width / 2, y: to.y + to.height / 2 }],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchMove",
    touchPoints: [
      { x: back.x + back.width / 2, y: back.y + back.height / 2 },
    ],
  });
  await client.send("Input.dispatchTouchEvent", {
    type: "touchEnd",
    touchPoints: [],
  });
  await expect(page.locator(".square.pending")).toHaveCount(0);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

test("live preview stays separate and direct keyboard blanks survive saving", async ({
  page,
}) => {
  await ready(page);
  await newGame(page, "?AEINRT");
  await page.locator('[data-row="7"][data-col="7"]').click();
  await page.keyboard.press("Shift+O");
  await page.keyboard.type("at");
  await expect(page.locator("#move-feedback")).toContainText("4 points");
  await expect(page.locator("#history-position option")).toHaveCount(1);
  await page.keyboard.press("Enter");
  await expect(page.locator("#history-position option")).toHaveCount(2);
  await expect(page.locator(".square.blank.owner-0")).toHaveCount(1);
  await page.locator("#rack").fill("AEINRST");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator('[data-row="8"][data-col="7"]').click();
  await page.keyboard.type("at");
  await expect(page.locator("#move-feedback")).toContainText("points");
  await page.screenshot({
    path: test.info().outputPath("move-entry-desktop.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 375, height: 844 });
  await page.screenshot({
    path: test.info().outputPath("move-entry-mobile.png"),
    fullPage: true,
  });
});

test("desktop context hints follow hover, focus and board mode; mobile keeps controls help", async ({
  page,
}) => {
  await ready(page);
  const hint = page.locator("#context-status");
  await expect(hint).toBeVisible();
  await expect(page.locator(".move-controls-help")).toBeHidden();
  await page.locator("#rack").hover();
  await expect(hint).toContainText("Use ? for a blank");
  await page.locator("#opponent-rack").focus();
  await expect(hint).toContainText("leave unknown tiles empty");
  await page.locator("#open-analysis-settings").click();
  await expect(hint).toBeEmpty();
  await page.locator("#plies").focus();
  await expect(hint).toContainText("turns ahead");
  await page.locator("#back-to-game").click();
  await page.locator('[data-row="7"][data-col="7"]').hover();
  await expect(hint).toContainText("enable Edit board");
  await page.locator("#edit").check();
  await page.locator('[data-row="7"][data-col="7"]').click();
  await expect(hint).toContainText("Type to edit");
  await page.keyboard.type("a");
  await expect(hint).toContainText("Type to edit");
  await page.locator("#edit").uncheck();
  await newGame(page, "AEINRST");
  await page.locator('[data-row="7"][data-col="7"]').click();
  await page.keyboard.type("at");
  await expect(hint).toContainText("Enter: play");
  await expect(page.locator("#move-feedback")).toContainText("points");
  await page.screenshot({
    path: test.info().outputPath("context-hints-desktop.png"),
  });
  await page.setViewportSize({ width: 375, height: 844 });
  await expect(hint).toBeHidden();
  await expect(page.locator(".move-controls-help")).toBeVisible();
  await page.locator(".move-controls-help summary").click();
  await expect(page.locator(".move-controls-help")).toContainText(
    "Shift + letter",
  );
});

test("tile entry does not also trigger global analysis shortcuts", async ({
  page,
}) => {
  await ready(page);
  await newGame(page, "SKEPAAA");
  await page.locator('[data-row="7"][data-col="7"]').click();
  for (const letter of "skep") {
    await page.keyboard.press(letter);
    await expect(page.locator("body")).not.toHaveClass(/busy/, {
      timeout: 500,
    });
  }
  await expect(page.locator(".square.pending")).toHaveCount(4);
  await expect(page.locator("#results tbody tr")).toHaveCount(0);
});

test("game setup, stable scoreboard and responsive tracking", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1640, height: 950 });
  await ready(page);
  await page.locator("#position-editor > summary").click();
  await expect(page).toHaveTitle("Magpie");
  await expect(page.locator(".badge")).toHaveText("Beta");
  await page.locator("#new-game").click();
  await expect(
    page.getByRole("button", { name: "Play Magpie", exact: true }),
  ).toHaveAttribute("aria-pressed", "true");
  await expect(page.locator("#player-one-name")).toHaveValue("You");
  await expect(page.locator("#player-two-name")).toHaveValue(
    "Magpie",
  );
  await page.locator(".game-options > summary").click();
  await expect(page.locator("#watch-delay")).not.toBeVisible();
  await page
    .getByRole("button", { name: "Watch computers", exact: true })
    .click();
  await expect(page.locator("#human-side")).not.toBeVisible();
  await expect(page.locator("#watch-delay")).toBeVisible();
  await page
    .getByRole("button", { name: "Play Magpie", exact: true })
    .click();
  await page.selectOption("#bot-analysis", "kibitz");
  await page.selectOption("#game-time", "180");
  await page.locator("#new-game-form button.primary").click();
  await expect(page.locator("#session-result")).toHaveText(
    "Your turn",
  );
  await expect(page.locator("#name-one")).toHaveText("You");
  await expect(page.locator("#name-two")).toHaveText("Magpie");
  const rack = await page.locator("#rack-tiles").textContent();
  expect(
    await page
      .locator("#move-entry")
      .evaluate(
        (el) => el.getBoundingClientRect().bottom < innerHeight - 32,
      ),
  ).toBe(true);
  await page.locator("#pass-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(
    3,
    { timeout: 30000 },
  );
  await page.locator("#pause-game").click();
  await expect(page.locator("#name-one")).toHaveText("You");
  await expect(page.locator("#name-two")).toHaveText("Magpie");
  await expect(page.locator("#points-one")).toHaveText("0");
  expect(
    Number(await page.locator("#points-two").textContent()),
  ).toBeGreaterThan(0);
  await expect(page.locator("#rack-tiles")).toHaveText(rack);
  await expect(page.locator("#position-editor")).not.toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.locator("#unseen-tiles")).not.toBeVisible();
  await expect(page.locator("#unseen-counts")).toBeVisible();
  await expect(
    page.locator(".move-controls-help > summary"),
  ).toBeVisible();
  await page.locator("#unseen-pool > summary").click();
  await expect(page.locator("#unseen-tiles")).toBeVisible();
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

const chooserFreeGame = "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Alice\n#player2 p2 Bob\n>p1: AEFAULT 8D FAULT +24 24\n>p2: ABELNRW E7 N.RWAL +18 18\n";
async function openGameDialog(page) {
  await page.locator("#game-menu > summary").click();
  await page.locator("#load-gcg").click();
  await expect(page.locator("#gcg-dialog")).toBeVisible();
}
test("GCG paste bypasses the native picker and retains a game on invalid input", async ({ page }) => {
  await ready(page);
  let pickers = 0;
  page.on("filechooser", () => pickers++);
  await openGameDialog(page);
  expect(pickers).toBe(0);
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
  await page.locator("#history-last").click();
  await expect(page.locator(".square.played")).toHaveCount(10);
  await openGameDialog(page);
  await page.locator("#gcg-text").fill("#lexicon INVALID\n");
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-load-error")).toContainText("INVALID");
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
  await expect(page.locator(".square.played")).toHaveCount(10);
  expect(pickers).toBe(0);
});
test("GCG dialog accepts dropped files and explicitly chosen files", async ({ page }) => {
  await ready(page);
  await openGameDialog(page);
  const transfer = await page.evaluateHandle((text) => {
    const transfer = new DataTransfer();
    transfer.items.add(new File([text], "dropped.gcg", { type: "text/plain" }));
    return transfer;
  }, chooserFreeGame);
  await page.locator("#gcg-drop-zone").dispatchEvent("drop", { dataTransfer: transfer });
  await expect(page.locator("#status")).toContainText("Loaded dropped.gcg");
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await openGameDialog(page);
  const chooser = page.waitForEvent("filechooser");
  await page.locator("#choose-gcg").click();
  await (await chooser).setFiles({ name: "chosen.gcg", mimeType: "text/plain", buffer: Buffer.from(chooserFreeGame) });
  await expect(page.locator("#status")).toContainText("Loaded chosen.gcg");
  await expect(page.locator("#history-position option")).toHaveCount(3);
});
test("stalled cloud-file reads time out and pasted GCG still works", async ({ page }) => {
  await page.addInitScript(() => {
    const NativeWorker = window.Worker;
    window.Worker = class extends NativeWorker {
      constructor(url, options) {
        if (String(url).includes("gcg-loader-worker")) {
          const original = new URL(url, location.href).href;
          url = URL.createObjectURL(new Blob([
            'File.prototype.arrayBuffer = () => new Promise(() => {}); importScripts(' + JSON.stringify(original) + ');'
          ], {type:"text/javascript"}));
        }
        super(url, options);
      }
    };
  });
  await ready(page);
  await openGameDialog(page);
  await page.locator("#gcg-file").setInputFiles({ name: "stalled.gcg", mimeType: "text/plain", buffer: Buffer.from(chooserFreeGame) });
  await expect(page.locator("#gcg-load-error")).toContainText("File read timed out", { timeout: 15000 });
  await expect(page.locator("#submit-gcg")).toBeEnabled();
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
});

test("a blocked GCG worker leaves the page responsive and is replaced on retry", async ({ page }) => {
  await page.addInitScript(() => {
    const NativeWorker = window.Worker;
    window.Worker = class extends NativeWorker {
      constructor(url, options) {
        if (window.blockNextGCG && String(url).includes("gcg-loader-worker")) {
          window.blockNextGCG = false;
          url = URL.createObjectURL(new Blob(['self.onmessage = () => { while (true) {} };'], {type:"text/javascript"}));
        }
        super(url, options);
      }
    };
  });
  await ready(page);
  await page.evaluate(() => { window.blockNextGCG = true; });
  await openGameDialog(page);
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  // Real UI interaction must work even while the separate worker is spinning.
  await page.locator("#close-gcg").click({ timeout: 2000 });
  await expect(page.locator("#gcg-dialog")).not.toBeVisible({ timeout: 2000 });
  await expect(page.locator("#status")).toContainText("Game could not be loaded", { timeout: 15000 });
  await openGameDialog(page);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
});

test("GCG loader preserves legacy encoding and rejects excessive input", async ({ page }) => {
  await ready(page);
  const result = await page.evaluate(async (game) => {
    const { readGCG } = await import("/wasmentry/gcg-loader.mjs");
    const legacy = game.replace("UTF-8", "ISO-8859-1").replace("Alice", "André");
    const file = new File([Uint8Array.from(legacy, (letter) => letter.charCodeAt(0))], "legacy.gcg");
    const decoded = await readGCG(file, "", "NWL23");
    const errors = [];
    for (const text of ["x".repeat(1024 * 1024 + 1), ">event\n".repeat(1001), "bad\0text"]) {
      try { await readGCG(null, text, "CSW24"); } catch (failure) { errors.push(failure.message); }
    }
    return { decoded, errors };
  }, chooserFreeGame);
  expect(result.decoded.text).toContain("#character-encoding UTF-8");
  expect(result.decoded.text).toContain("André");
  expect(result.decoded.lexicon).toBe("CSW24");
  expect(result.errors).toEqual([
    "GCG files must be smaller than 1 MB.",
    "GCG files may contain at most 1,000 events.",
    "The file is not a text GCG.",
  ]);
});

test("GCG diagnostics persist the native chooser request before dispatch", async ({ page }) => {
  await ready(page);
  await openGameDialog(page);
  const chooser = page.waitForEvent("filechooser");
  await page.locator("#choose-gcg").click();
  const picker = await chooser;
  const beforeSelection = await page.evaluate(() => localStorage.getItem("magpie-gcg-diagnostics-v1"));
  expect(beforeSelection).toContain("chooser.request");
  expect(beforeSelection).not.toContain("chooser.selection");
  await picker.setFiles({ name: "private-name.gcg", mimeType: "text/plain", buffer: Buffer.from(chooserFreeGame) });
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  const trace = await page.evaluate(() => localStorage.getItem("magpie-gcg-diagnostics-v1"));
  for (const stage of ["chooser.selection", "loader.received", "file.read", "text.decode", "engine.import", "load.complete"]) {
    expect(trace).toContain(stage);
  }
  expect(trace).not.toContain("private-name");
  expect(trace).not.toContain("FAULT");
  await page.reload();
  await expect(page.locator("#status")).toHaveText("Engine ready");
  await openGameDialog(page);
  await page.locator("#gcg-diagnostics > summary").click();
  await expect(page.locator("#gcg-diagnostics-log")).toContainText("load.complete");
  await page.evaluate(() => {
    Object.defineProperty(navigator.clipboard, "writeText", { configurable: true, value: async text => { window.copiedGCGLog = text; } });
  });
  await page.locator("#copy-gcg-diagnostics").click();
  expect(await page.evaluate(() => window.copiedGCGLog)).toContain("chooser.request");
});

test("GCG diagnostics retain a failure stage and tolerate unavailable storage", async ({ page }) => {
  await ready(page);
  await page.evaluate(() => {
    Storage.prototype.setItem = () => { throw new Error("storage blocked"); };
  });
  await openGameDialog(page);
  await page.locator("#gcg-text").fill("#lexicon INVALID\n");
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-diagnostics-log")).toBeVisible();
  await expect(page.locator("#gcg-diagnostics-log")).toContainText("load.failed");
  await expect(page.locator("#gcg-diagnostics-log")).toContainText("loader.failed");
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
});

test("GCG worker remains compatible with a loader from an already-open tab", async ({ page }) => {
  // The original loader treats the first worker message as the final result.
  await page.route("**/gcg-loader.mjs*", route => route.fulfill({
    contentType: "text/javascript",
    body: `export function readGCG(file, pastedText, defaultLexicon) {
      return new Promise((resolve, reject) => {
        const worker = new Worker(new URL("./gcg-loader-worker.js", import.meta.url));
        worker.onmessage = ({data}) => {
          worker.terminate();
          data.error ? reject(new Error(data.error)) : resolve(data);
        };
        worker.postMessage({file, pastedText, defaultLexicon});
      });
    }`,
  }));
  const invalidRequests = [];
  page.on("request", request => {
    if (request.url().includes("undefined.kwg")) invalidRequests.push(request.url());
  });
  await ready(page);
  await openGameDialog(page);
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
  expect(invalidRequests).toEqual([]);
});


test("GCG without a lexicon header uses the selected lexicon", async ({ page }) => {
  await ready(page);
  await openGameDialog(page);
  await page.locator("#gcg-text").fill(chooserFreeGame.replace("#lexicon CSW24\n", ""));
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-dialog")).not.toBeVisible();
  await expect(page.locator("#current-lexicon")).toHaveText("CSW24");
  await expect(page.locator("#game-players")).toHaveText("Alice vs Bob");
  const fallback = await page.evaluate(async (text) => {
    const { readGCG } = await import("/wasmentry/gcg-loader.mjs?v=2");
    return (await readGCG(null, text, "NWL23")).lexicon;
  }, chooserFreeGame.replace("#lexicon CSW24\n", ""));
  expect(fallback).toBe("NWL23");
});

test("GCG loader rejects incomplete results before engine setup", async ({ page }) => {
  await page.addInitScript(() => {
    const NativeWorker = window.Worker;
    window.Worker = class extends NativeWorker {
      constructor(url, options) {
        if (String(url).includes("gcg-loader-worker")) url = URL.createObjectURL(new Blob([
          'self.onmessage = () => self.postMessage({});'
        ], {type:"text/javascript"}));
        super(url, options);
      }
    };
  });
  const invalidRequests = [];
  page.on("request", request => {
    if (request.url().includes("undefined.kwg")) invalidRequests.push(request.url());
  });
  await ready(page);
  await openGameDialog(page);
  await page.locator("#gcg-text").fill(chooserFreeGame);
  await page.locator("#submit-gcg").click();
  await expect(page.locator("#gcg-load-error")).toContainText("incomplete result");
  await expect(page.locator("#submit-gcg")).toBeEnabled();
  expect(invalidRequests).toEqual([]);
});


test("exchange selector handles duplicate tiles, blanks, cancellation and history editing", async ({ page }) => {
  await ready(page);
  await newGame(page, "AAEIRT?");
  await expect(page.locator("#move-entry input")).toHaveCount(0);
  await page.locator("#exchange-move").click();
  await page.locator('#exchange-tiles [data-letter="A"]').first().click();
  await page.locator("#cancel-exchange").click();
  await expect(page.locator("#history-position option")).toHaveCount(1);
  await page.locator("#exchange-move").click();
  await expect(page.locator("#confirm-exchange")).toBeDisabled();
  await page.locator('#exchange-tiles [data-letter="A"]').first().click();
  await page.locator('#exchange-tiles [data-letter="?"]').click();
  await page.locator("#confirm-exchange").click();
  await expect(page.locator("#history-position option")).toHaveCount(2);
  await page.locator("#edit-history-move").click();
  await expect(page.locator('#exchange-tiles [aria-pressed="true"]')).toHaveCount(2);
  await expect(page.locator('#exchange-tiles [data-letter="A"][aria-pressed="true"]')).toHaveCount(1);
  await expect(page.locator('#exchange-tiles [data-letter="?"]')).toHaveAttribute("aria-pressed", "true");
  await page.locator("#confirm-exchange").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#status")).toHaveText("Move recorded");
  await expect(page.locator("#history-position option")).toHaveCount(2);
});
