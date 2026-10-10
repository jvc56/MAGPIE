import { test, expect } from "@playwright/test";

test("wide layout keeps rack controls in view and settings between sidebars", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
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
      rack: rect(".rack-actions"),
      status: rect("#context-status"),
    };
  });
  expect(bounds.brand.right).toBeLessThan(bounds.position.x);
  expect(bounds.position.right).toBeLessThan(bounds.analysis.x);
  expect(bounds.analysis.right).toBeLessThan(bounds.actions.x);
  expect(bounds.rack.bottom).toBeLessThan(bounds.status.y);
  expect(bounds.position.y).toBe(bounds.analysis.y);
  await page.screenshot({
    path: test.info().outputPath("four-panel-desktop.png"),
  });
  await page.locator("#open-settings").click();
  await expect(page.locator("#settings-view")).toBeVisible();
  await expect(page.locator("#game-view")).toBeHidden();
  await page.locator("#back-to-game").click();
  await expect(page.locator("#sort-rack")).toBeInViewport();
  await page.setViewportSize({ width: 1536, height: 768 });
  expect(
    await page
      .locator(".rack-actions")
      .evaluate((el) => el.getBoundingClientRect().bottom),
  ).toBeLessThan(734);
  for (const width of [1280, 850, 390, 375]) {
    await page.setViewportSize({ width, height: 844 });
    expect(
      await page.evaluate(() => document.documentElement.scrollWidth),
    ).toBe(width);
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
  await page.locator("#position-editor > summary").click();
  await expect(page.locator(".analysis-actions kbd")).toHaveCount(0);
  for (const sample of ["opening", "peg", "endgame"]) {
    await page.selectOption("#sample", sample);
    await page.locator('[data-mode="kibitz"]').focus();
    await page.keyboard.type("kspeKSPE");
    await expect(page.locator("#stop")).toBeDisabled();
    await expect(page.locator("#results tbody tr")).toHaveCount(0);
  }
});
