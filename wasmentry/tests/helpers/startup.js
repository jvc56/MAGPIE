import { expect } from "@playwright/test";
import { SAMPLES } from "../../analysis-model.mjs";

// Fixtures use the same CGP import flow available to users.
export async function loadSample(page, name) {

  await page.locator('#import').click();
  await page.locator('#cgp').fill(SAMPLES[name]);
  await page.locator('#import-form button[type="submit"]').click();
  await expect(page.locator('#import-dialog')).toBeHidden();
}

export async function openSample(page) {
  await page.locator('#cancel-new-game').click();
  await loadSample(page, 'opening');
}
