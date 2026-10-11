import { test, expect } from '@playwright/test';

async function ready(page) {
  await page.goto('/wasmentry/');
  await expect(page.getByRole('dialog', {name:'New game', exact:true})).toBeVisible();
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(0);
  await expect(page.locator('#board .played')).toHaveCount(0);
}

test('startup offers game choices without dealing an example rack, including on mobile', async ({page}) => {
  await ready(page);
  for (const name of ['Play Magpie', 'Record a game', 'Watch computers', 'Open game…'])
    await expect(page.getByRole('button', {name,exact:true})).toBeVisible();
  await page.screenshot({path:test.info().outputPath('new-game-desktop.png')});
  await page.setViewportSize({width:390,height:844});
  await expect(page.locator('#start-game')).toBeInViewport();
  await expect(page.locator('#new-game-open')).toBeInViewport();
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  await page.screenshot({path:test.info().outputPath('new-game-mobile.png')});
  await page.keyboard.press('Escape');
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(0);
  await page.locator('#new-game').click();
  await expect(page.locator('#new-game-dialog')).toBeVisible();
});

for (const kind of ['play', 'watch', 'record']) test(`start ${kind} directly from startup`, async ({page}) => {
  await ready(page);
  await page.locator(`[data-game-kind=${kind}]`).click();
  if (kind !== 'record') {
    await page.locator('.game-options > summary').click();
    await page.locator('#bot-analysis').selectOption('kibitz');
    await page.locator('#game-time').selectOption('0');
  }
  await page.locator('#start-game').click();
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  if (kind === 'record') {
    await expect(page.locator('#status')).toHaveText('Game ready · enter the first rack');
    await expect(page.locator('#rack')).toBeFocused();
    await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(0);
  } else {
    await expect(page.locator('#play-session')).toBeVisible();
    if (kind === 'play') await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(7);
    else await expect(page.locator('#history-position option').nth(1)).toBeAttached({timeout:30000});
  }
  await expect(page.locator('#error')).toBeHidden();
});

test('a saved game can be opened directly from startup without starting a new game', async ({page}) => {
  await ready(page);
  await page.locator('#new-game-open').click();
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  await page.locator('#gcg-text').fill('#lexicon CSW24\n#player1 p1 Ada\n#player2 p2 Bo\n>p1: AEFAULT 8D FAULT +24 24\n');
  await page.locator('#submit-gcg').click();
  await expect(page.locator('#gcg-dialog')).toBeHidden({timeout:30000});
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await expect(page.locator('#error')).toBeHidden();
});

test('engine startup failure is visible in the choice dialog', async ({page}) => {
  await page.route('**/wasm-worker.js', route => route.abort());
  await page.goto('/wasmentry/');
  await expect(page.locator('#new-game-status')).toContainText('Engine unavailable', {timeout:30000});
  await expect(page.locator('#start-game')).toBeDisabled();
  await expect(page.locator('#new-game-dialog')).toBeVisible();
});
