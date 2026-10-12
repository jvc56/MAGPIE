import { test, expect } from '@playwright/test';

for (const submit of ['Enter', 'button']) {
  for (const col of [7, 8]) {
    test(`rejected phony stays keyboard editable after ${submit}, starting column ${col}`, async ({page}) => {
      await page.goto('/wasmentry/');
      await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
      await page.locator('#new-game-position > summary').click();
      const rows = Array(15).fill('15');
      if (col === 8) rows[7] = '7A7';
      await page.locator('#new-game-cgp').fill(
        `${rows.join('/')} AEINRST/BCDFGHJ 0/0 0 -lex CSW24`,
      );
      await page.locator('.game-options > summary').click();
      await page.locator('#game-time').selectOption('0');
      await page.locator('#challenge-rule').selectOption('void');
      await page.locator('#start-game').click();
      await expect(page.locator('#new-game-dialog')).toBeHidden();
      await page.locator(`[data-row="7"][data-col="${col}"]`).click();
      await page.keyboard.type('tsraine');
      await expect(page.locator('.square.pending')).toHaveCount(7);
      if (submit === 'Enter') await page.keyboard.press('Enter');
      else await page.locator('#play-move').click();
      await expect(page.locator('#error')).toContainText('phony');
      if (col === 8) await expect(page.locator('#error')).toContainText('8H (A)TSRAINE');
      await expect(page.locator('#play-move')).toBeEnabled();
      await page.keyboard.press('Backspace');
      await expect(page.locator('.square.pending')).toHaveCount(6);
      await expect(page.locator('#rack-tiles [data-letter="E"]')).toHaveCount(1);
      await expect(page.locator('#error')).toBeHidden();
      await expect(page.locator('#history-position option')).toHaveCount(1);
      await page.keyboard.type('e');
      await expect(page.locator('.square.pending')).toHaveCount(7);
      await page.keyboard.press('Escape');
      await expect(page.locator('.square.pending')).toHaveCount(0);
      await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(7);
    });
  }
}
