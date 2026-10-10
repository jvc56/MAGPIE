import { test, expect } from "@playwright/test";
import { formatHistoryPlay, parseCGP, SAMPLES } from "../analysis-model.mjs";

test("history notation groups existing tiles and preserves blanks and special events", () => {
  const position = parseCGP(SAMPLES.opening);
  position.board[7][4] = "A";
  expect(formatHistoryPlay(">p2: ABELNRW E7 N.RWAL +18 18", position)).toBe(
    "E7 N(A)RWAL +18 18",
  );
  position.board[7][5] = "t";
  expect(formatHistoryPlay(">p1: ER 8D E..R +4 28", position)).toBe(
    "8D E(At)R +4 28",
  );
  expect(formatHistoryPlay(">p1: AE - +0 24", position)).toBe("- +0 24");
  expect(formatHistoryPlay(">p1: AE -AE +0 24", position)).toBe("-AE +0 24");
  expect(formatHistoryPlay(">p1: AE -- -24 0", position)).toBe("-- -24 0");
});

async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
}

test("history pairs players and shows playthrough notation in rows and navigation", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await ready(page);
  const gcg =
    "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Player 1\n#player2 p2 Player 2\n>p1: AEFAULT 8D FAULT +24 24\n>p2: ABELNRW E7 N.RWAL +18 18\n";
  await page
    .locator("#gcg-file")
    .setInputFiles({
      name: "narwal.gcg",
      mimeType: "text/plain",
      buffer: Buffer.from(gcg),
    });
  await expect(page.locator("#history-table tbody tr")).toHaveCount(1);
  const cells = page.locator("#history-table tbody tr").first().locator("td");
  await expect(cells.nth(1)).toContainText("8D FAULT");
  await expect(cells.nth(2)).toContainText("E7 N(A)RWAL");
  await expect(
    page.locator('#history-position option[value="2"]'),
  ).toContainText("N(A)RWAL");
  await page.locator('#history-table [data-event-index="1"]').click();
  await expect(page.locator("#history-position")).toHaveValue("1");
  await expect(page.locator(".square.played")).toHaveCount(5);
  await page.locator("#hide-spoilers").check();
  await expect(cells.nth(2)).toHaveText("•••");
  await page.locator("#hide-spoilers").uncheck();
});

test("computer game history occupies the right panel without overlapping the board", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await ready(page);
  await page.locator("#open-settings").click();
  await page.selectOption("#theme", "light");
  await page.locator("#back-to-game").click();
  await page.locator("#new-game").click();
  await page.locator(".game-options > summary").click();
  await page.selectOption("#bot-analysis", "kibitz");
  await page.locator("#new-game-form button.primary").click();
  await page.locator("#pass-move").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.locator("#pause-game").click();
  for (const width of [1936, 1536, 1280]) {
    await page.setViewportSize({ width, height: 890 });
    await expect.poll(() => page.evaluate(() => {
      const board = document.querySelector(".position").getBoundingClientRect();
      const history = document.querySelector("#game-history").getBoundingClientRect();
      return {right: history.x >= board.right, aligned: history.y === board.y};
    })).toEqual({right:true, aligned:true});
  }
  await page.setViewportSize({ width: 1936, height: 890 });
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.screenshot({
    path: test.info().outputPath("history-right-desktop.png"),
  });
  await page.locator("#show-live-analysis").check();
  const analysis = await page.locator(".analysis").boundingBox();
  const history = await page.locator("#game-history").boundingBox();
  expect(history.x).toBe(analysis.x);
  expect(history.y).toBeGreaterThanOrEqual(analysis.y + analysis.height);
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(
    390,
  );
  await page.locator("#show-live-analysis").uncheck();
  await page.screenshot({
    path: test.info().outputPath("history-right-mobile.png"),
    fullPage: true,
  });
});
