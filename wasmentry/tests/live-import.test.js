import { test, expect } from '@playwright/test';
import { readFile } from 'node:fs/promises';

async function startGame(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await page.locator('#new-game').click();
  await page.locator('[data-game-kind=play]').click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#first-player', '0');
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#play-session')).toBeVisible();
}

for (const source of ['paste', 'file', 'drop']) {
  test(`GCG ${source} asks before replacing a live game; cancel preserves it`, async ({ page }) => {
    await startGame(page);
    const before = await page.locator('#rack-tiles').textContent();
    const text = await readFile('tests/fixtures/standard.gcg', 'utf8');
    await page.locator('#game-menu > summary').click();
    await page.locator('#load-gcg').click();
    const load = async () => {
      if (source === 'paste') {
        await page.locator('#gcg-text').fill(text);
        await page.locator('#submit-gcg').click();
      } else if (source === 'file') {
        await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
      } else {
        await page.locator('#gcg-drop-zone').evaluate((zone, text) => {
          const transfer = new DataTransfer();
          transfer.items.add(new File([text], 'review.gcg', { type: 'text/plain' }));
          zone.dispatchEvent(new DragEvent('drop', { bubbles: true, dataTransfer: transfer }));
        }, text);
      }
    };
    await load();
    await expect(page.locator('#game-confirm')).toBeVisible();
    await expect(page.locator('#confirm-title')).toHaveText('Open another game?');
    await page.locator('#confirm-no').click();
    await expect(page.locator('#play-session')).toBeVisible();
    await expect(page.locator('#rack-tiles')).toHaveText(before);
    await expect(page.locator('#history-position option')).toHaveCount(1);
    await load();
    await page.locator('#confirm-yes').click();
    await expect(page.locator('#gcg-dialog')).not.toBeVisible();
    await expect(page.locator('#status')).toContainText('Loaded');
    await expect(page.locator('#play-session')).not.toBeVisible();
    expect(await page.locator('#history-position option').count()).toBeGreaterThan(1);
  });
}

test('a failed confirmed import leaves the live game playable', async ({ page }) => {
  await startGame(page);
  const before = await page.locator('#rack-tiles').textContent();
  await page.locator('#game-menu > summary').click();
  await page.locator('#load-gcg').click();
  await page.locator('#gcg-text').fill('not a game');
  await page.locator('#submit-gcg').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#gcg-load-error')).not.toBeEmpty();
  await page.locator('#close-gcg').click();
  await expect(page.locator('#play-session')).toBeVisible();
  await expect(page.locator('#rack-tiles')).toHaveText(before);
  await expect(page.locator('#pass-move')).toBeEnabled();
  await expect(page.locator('#pause-game')).toHaveText('Pause');
});
