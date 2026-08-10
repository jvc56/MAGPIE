import { test, expect } from "@playwright/test";

// End-to-end smoke tests for the mouse-oriented analysis board (webui/).
// Each test drives the full path: page -> worker -> WASM -> JSON exports -> UI.

const MIDGAME_CGP =
  "C14/O2TOY9/mIRADOR8/F4DAB2PUGH1/I5GOOEY3V/T4XI2MALTHA/14N/6GUM3OWN/7PEW2DOE/9EF1DOR/2KUNA1J1BEVELS/3TURRETs2S2/7A4T2/7N7/7S7 EEEIILZ/ 336/298 0 -lex NWL23";
const ENDGAME_CGP =
  "GATELEGs1POGOED/R4MOOLI3X1/AA10U2/YU4BREDRIN2/1TITULE3E1IN1/1E4N3c1BOK/1C2O4CHARD1/QI1FLAWN2E1OE1/IS2E1HIN1A1W2/1MOTIVATE1T1S2/1S2N5S4/3PERJURY5/15/15/15 FV/AADIZ 442/388 0 -lex CSW24";

async function waitReady(page) {
  const errors = [];
  page.on("pageerror", (e) => errors.push(String(e)));
  await page.goto("/webui/");
  await expect(page.locator("#status")).toContainText("Ready", {
    timeout: 60000,
  });
  await expect(page.locator("#board .cell")).toHaveCount(225);
  return errors;
}

test.describe("MAGPIE web UI", () => {
  test("loads, generates plays, previews and selects a move; fits the window", async ({
    page,
  }) => {
    test.setTimeout(120000);
    const errors = await waitReady(page);

    // Everything fits in the viewport — no page scroll.
    const overflow = await page.evaluate(
      () =>
        document.documentElement.scrollHeight -
        document.documentElement.clientHeight,
    );
    expect(overflow).toBeLessThanOrEqual(2);

    // Set the on-turn rack and generate.
    await page.locator(".player-card.on-turn .rack-input").fill("AEINRST");
    await page.click("#btn-generate");

    await expect(page.locator("#moves-body tr td.play").first()).not.toHaveText(
      "",
      { timeout: 60000 },
    );
    expect(await page.locator("#moves-body tr").count()).toBeGreaterThan(5);
    await expect(page.locator("#status")).toContainText("plays");

    // Hover previews placed tiles (ghosts); clicking pins the highlight.
    await page.locator("#moves-body tr").first().hover();
    await expect(page.locator("#board .cell.ghost").first()).toBeVisible();
    await page.locator("#moves-body tr").first().click();
    await expect(page.locator("#board .cell.highlight").first()).toBeVisible();

    expect(errors, errors.join("\n")).toEqual([]);
  });

  test("loads a CGP via the inline field", async ({ page }) => {
    test.setTimeout(120000);
    const errors = await waitReady(page);

    await page.click("#btn-load-cgp");
    await page.locator("#cgp-input").fill(MIDGAME_CGP);
    await page.click("#cgp-load-btn");

    // Lexicon switched, the on-turn rack and a board full of tiles loaded.
    await expect(page.locator("#lexicon")).toHaveValue("NWL23", {
      timeout: 60000,
    });
    await expect(page.locator(".player-card.on-turn .rack-input")).toHaveValue(
      "EEEIILZ",
    );
    expect(await page.locator("#board .cell.tile").count()).toBeGreaterThan(20);

    // Tiles show point values (Woogles-style subscripts).
    expect(await page.locator("#board .cell.tile .pt").count()).toBeGreaterThan(
      0,
    );

    expect(errors, errors.join("\n")).toEqual([]);
  });

  test("plays through a game (advances the position move by move)", async ({
    page,
  }) => {
    test.setTimeout(120000);
    const errors = await waitReady(page);

    // Load an endgame position where both players have racks.
    await page.click("#btn-load-cgp");
    await page.locator("#cgp-input").fill(ENDGAME_CGP);
    await page.click("#cgp-load-btn");
    await expect(page.locator("#lexicon")).toHaveValue("CSW24", {
      timeout: 60000,
    });

    const tileCount = () => page.locator("#board .cell.tile").count();
    const before = await tileCount();

    // Generate, then commit the top play.
    await page.click("#btn-generate");
    await expect(page.locator("#moves-body tr td.play").first()).not.toHaveText(
      "",
      { timeout: 60000 },
    );
    await page.locator('#moves-body .mini[data-act="play"]').first().click();

    // The board gained the committed tiles and the play list cleared.
    await expect
      .poll(tileCount, { timeout: 60000 })
      .toBeGreaterThan(before);
    await expect(page.locator("#moves-body td.empty")).toBeVisible();

    // Now the other player is on turn — generate and commit again.
    const afterFirst = await tileCount();
    await page.click("#btn-generate");
    await expect(page.locator("#moves-body tr td.play").first()).not.toHaveText(
      "",
      { timeout: 60000 },
    );
    await page.locator('#moves-body .mini[data-act="play"]').first().click();
    await expect
      .poll(tileCount, { timeout: 60000 })
      .toBeGreaterThan(afterFirst);

    expect(errors, errors.join("\n")).toEqual([]);
  });
});
