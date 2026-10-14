import { openSample, loadSample } from "./helpers/startup.js";
import { test, expect } from "@playwright/test";

test("wide layout keeps panels separate and settings between sidebars", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await openSample(page);
  const bounds = await page.evaluate(() => {
    const rect = (selector) => {
      const { x, y, right, bottom } = document
        .querySelector(selector)
        .getBoundingClientRect();
      return { x, y, right, bottom };
    };
    return {
      brand: rect(".brand"),
      position: rect(".position"),
      analysis: rect(".analysis"),
      actions: rect(".header-actions"),
    };
  });
  expect(bounds.brand.right).toBeLessThan(bounds.position.x);
  expect(bounds.position.right).toBeLessThan(bounds.analysis.x);
  expect(bounds.analysis.right).toBeLessThan(bounds.actions.x);
  expect(bounds.position.right - bounds.position.x).toBeGreaterThanOrEqual(640);
  expect(bounds.position.y).toBe(bounds.analysis.y);
  await page.screenshot({
    path: test.info().outputPath("four-panel-desktop.png"),
  });
  await page.locator("#open-settings").click();
  await expect(page.locator("#settings-view")).toBeVisible();
  await expect(page.locator("#game-view")).toBeHidden();
  await page.locator("#back-to-game").click();
  await page.locator('#sort-rack').scrollIntoViewIfNeeded();
  await expect(page.locator('#sort-rack')).toBeInViewport();
  await page.setViewportSize({ width: 1536, height: 768 });
  await page.locator('#sort-rack').scrollIntoViewIfNeeded();
  await expect(page.locator('#sort-rack')).toBeInViewport();
  for (const width of [1280, 850, 390, 375]) {
    await page.setViewportSize({ width, height: 844 });
    await expect.poll(() => page.evaluate(() => document.documentElement.scrollWidth)).toBe(width);
    await expect(page.locator("#open-settings")).toBeVisible();
  }
  await page.screenshot({
    path: test.info().outputPath("four-panel-mobile.png"),
    fullPage: true,
  });
});

test("letter keys outside tile entry never start analysis", async ({
  page,
}) => {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await openSample(page);
  await page.locator("#position-editor > summary").click();
  await expect(page.locator(".analysis-actions kbd")).toHaveCount(0);
  for (const sample of ["opening", "peg", "endgame"]) {
    await loadSample(page, sample);
    await page.locator('[data-mode="kibitz"]').focus();
    await page.keyboard.type("kspeKSPE");
    await expect(page.locator("#stop")).toBeDisabled();
    await expect(page.locator("#results tbody tr")).toHaveCount(0);
  }
});

for (const width of [900, 1936]) {
  test(`board width stays stable through content changes at ${width}px`, async ({page}) => {
    await page.setViewportSize({width, height: 890});
    await page.goto('/wasmentry/');
    await expect(page.locator('#status')).toHaveText('Engine ready', {timeout: 30000});
    await openSample(page);
    const board = page.locator('#board');
    const initialWidth = await board.evaluate(el => el.getBoundingClientRect().width);
    expect(initialWidth).toBeGreaterThanOrEqual(width === 900 ? 720 : 600);
    await page.evaluate(() => {
      window.boardWidths = [];
      window.boardObserver = new ResizeObserver(entries => {
        window.boardWidths.push(entries[0].contentRect.width);
      });
      window.boardObserver.observe(document.querySelector('#board'));
    });
    if (width <= 1000) await page.locator('.controls-help > summary').click();
    await page.locator('#position-editor > summary').click();
    // Exercise content growth independent of search timing: warnings, progress
    // and expanded outcomes must never feed back into the board's dimensions.
    await page.locator('#analysis-storage-warning').evaluate(el => {
      el.hidden = false;
      el.textContent = 'Long analysis warning. '.repeat(150);
    });
    await page.locator('[data-mode="kibitz"]').click();
    await expect(page.locator('#results tbody tr').first()).toBeVisible({timeout: 30000});
    await page.locator('#open-settings').click();
    await page.locator('#back-to-game').click();
    await page.locator('#new-game').click();
    await page.locator('#new-game-form button.primary').click();
    await expect(page.locator('#name-one')).toHaveText('You · Your turn');
    await page.locator('#show-live-analysis').check();
    await page.locator('#show-live-analysis').uncheck();
    await page.setViewportSize({width, height: 600});
    await expect.poll(() => board.evaluate(el => el.getBoundingClientRect().width)).toBe(initialWidth);
    const widths = await page.evaluate(() => {
      window.boardObserver.disconnect();
      return window.boardWidths.filter(value => value > 0); // Settings hides the game.
    });
    expect(widths.length).toBeGreaterThan(0);
    expect(Math.max(...widths) - Math.min(...widths)).toBeLessThan(1);
    await page.setViewportSize({width: 700, height: 600});
    await expect.poll(() => board.evaluate(el => el.getBoundingClientRect().width)).not.toBe(initialWidth);
  });
}
