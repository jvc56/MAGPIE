import { test, expect } from "@playwright/test";
import { inventory, parseCGP, SAMPLES } from "../analysis-model.mjs";

async function readPool(page) {
  return page
    .locator("#unseen-tiles span")
    .evaluateAll((tiles) =>
      Object.fromEntries(
        tiles.map((tile) => [tile.dataset.letter, Number(tile.dataset.count)]),
      ),
    );
}
async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await page.locator("#position-editor > summary").click();
}
test("unseen pool includes opposing racks and follows positions and layout", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await ready(page);
  await expect(page.locator("#brand-rail #unseen-pool")).toBeVisible();
  await expect(page.locator("#unseen-label")).toHaveText("Unseen tiles · 93");
  const opening = await readPool(page);
  await expect(page.locator("#unseen-counts")).toHaveText(
    "39 vowels 52 consonants 2 blanks",
  );
  expect(opening.A).toBe(8);
  expect(opening.E).toBe(11);
  expect(opening["?"]).toBe(2);
  await expect(page.locator("#unseen-tiles")).toHaveText(
    "AAAAAAAA EEEEEEEEEEE IIIIIIII OOOOOOOO UUUU BB CC DDDD FF GGG HH J K LLLL MM NNNNN PP Q RRRRR SSS TTTTT VV WW X YY Z ??",
  );
  await page.locator("#opponent-rack").fill("Z");
  await page.locator("#opponent-rack").blur();
  expect(await readPool(page)).toEqual(opening);
  for (const sample of ["midgame", "peg", "endgame"]) {
    await page.selectOption("#sample", sample);
    const { unseen, unseenCount } = inventory(parseCGP(SAMPLES[sample]));
    expect(await readPool(page)).toEqual(
      Object.fromEntries(Object.entries(unseen).filter(([, count]) => count)),
    );
    await expect(page.locator("#unseen-label")).toHaveText(
      `Unseen tiles · ${unseenCount}`,
    );
  }
  await page.selectOption("#sample", "opening");
  await expect(
    page.locator('#unseen-tiles [data-letter="?"]'),
  ).toBeInViewport();
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.screenshot({
    path: test.info().outputPath("unseen-desktop.png"),
  });
  await page.locator("#open-settings").click();
  await expect(page.locator("#unseen-pool")).toBeHidden();
  await page.locator("#back-to-game").click();
  await expect(page.locator("#unseen-pool")).toBeVisible();
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.locator("#unseen-home #unseen-pool")).toBeVisible();
  expect(await readPool(page)).toEqual(opening);
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(
    390,
  );
  await page.screenshot({
    path: test.info().outputPath("unseen-mobile.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 1936, height: 890 });
  await expect(page.locator("#brand-rail #unseen-pool")).toBeVisible();
});

test("pool stays with the human while the computer thinks", async ({
  page,
}) => {
  await ready(page);
  await page.locator("#new-game").click();
  await page.locator(".game-options > summary").click();
  await page.selectOption("#first-player", "0");
  await page.selectOption("#bot-analysis", "auto");
  await page.locator("#new-game-form button.primary").click();
  await expect(page.locator("#pass-move")).toBeEnabled();
  const before = await readPool(page);
  await page.locator("#pass-move").click();
  await expect(page.locator("#session-result")).toContainText("thinking");
  expect(await readPool(page)).toEqual(before);
  await expect(page.locator("#opponent-rack")).toBeHidden();
  await page.locator("#pause-game").click();
});

test("unassigned blanks use question marks and zero scores; designated blanks keep outlines", async ({
  page,
}) => {
  await ready(page);
  await expect(page.locator('#unseen-tiles [data-letter="?"]')).toHaveText(
    "??",
  );
  await page.locator("#rack").fill("AEINR??");
  await page.locator("#rack").blur();
  const blanks = page.locator('#rack-tiles [data-letter="?"]');
  await expect(blanks).toHaveCount(2);
  for (const tile of await blanks.all()) {
    await expect(tile.locator(".tile-letter")).toHaveText("?");
    await expect(tile.locator("sup")).toHaveText("0");
    await expect(tile.locator(".tile-letter")).toHaveCSS(
      "border-top-width",
      "0px",
    );
  }
  await expect(page.locator('#unseen-tiles [data-letter="?"]')).toHaveCount(0);
  await page.selectOption("#sample", "midgame");
  const designated = page.locator(".square.blank .tile-letter").first();
  await expect(designated).toHaveText("M");
  expect(
    await designated.evaluate((el) =>
      parseFloat(getComputedStyle(el).borderTopWidth),
    ),
  ).toBeGreaterThan(0);
});
