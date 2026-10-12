import { openSample } from "./helpers/startup.js";
import {test, expect} from '@playwright/test';
import {readFile} from 'node:fs/promises';

async function startWatch(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await openSample(page);
  await page.locator('#new-game').click();
  await page.locator('[data-game-kind=watch]').click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#game-time', '0');
  await page.selectOption('#bot-analysis', 'kibitz');
  await page.locator('#watch-delay').fill('2');
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await page.locator('#pause-game').click();
  await expect(page.locator('#pause-game')).toHaveText('Resume game');
}

async function finish(page) {
  await page.locator('#finish-game').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#session-result')).toContainText('Game ended');
}

async function exportedGame(page) {
  await page.locator('#game-menu > summary').click();
  const pending = page.waitForEvent('download');
  await page.locator('#save-gcg').click();
  return readFile(await (await pending).path(), 'utf8');
}

test('ending an idle game unlocks review controls without another engine command', async ({page}) => {
  await startWatch(page);
  await finish(page);
  await expect(page.locator('#session-controls')).toBeHidden();
  await expect(page.locator('#import')).toBeEnabled();
  await page.locator('#history-first').click();
  await page.locator('#hide-spoilers').check();
  await expect(page.locator('#session-result')).toBeHidden();
  await expect(page.locator('#history-position option').nth(1)).toHaveText('1. •••');
  await page.locator('#hide-spoilers').uncheck();
  await expect(page.locator('#session-result')).toBeVisible();
  for (const id of ['rack', 'opponent-rack', 'score', 'opponent-score', 'edit', 'clear', 'on-turn-player', 'play-move', 'shuffle-rack', 'sort-rack'])
    await expect(page.locator(`#${id}`)).toBeEnabled();
  await page.locator('#pass-move').click();
  await page.locator('#confirm-yes').click();
  await page.locator('#confirm-no').click();
  await expect(page.locator('#session-result')).toContainText('Game ended');
  await expect(page.locator('#history-position option')).toHaveCount(2);
});

test('a postmortem branch clears the old result in the UI and GCG; undo restores both', async ({page}) => {
  await startWatch(page);
  await finish(page);
  const result = await page.locator('#session-result').textContent();
  const original = await exportedGame(page);
  expect(original).toContain('#description Magpie finished:');
  await page.locator('#history-first').click();
  await page.locator('#pass-move').click();
  await page.locator('#confirm-yes').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#status')).toHaveText('Move recorded');
  await expect(page.locator('#play-session')).toBeHidden();
  await expect(page.locator('#points-one')).toHaveText('0');
  await expect(page.locator('#play-move')).toBeEnabled();
  const branch = await exportedGame(page);
  expect(branch).not.toContain('#description Magpie finished:');
  expect(branch).toMatch(/>p1:.* - \+0 0/);
  await page.locator('#undo-game').click();
  await expect(page.locator('#session-result')).toHaveText(result);
  expect(await exportedGame(page)).toBe(original);
  await page.screenshot({path:test.info().outputPath('finished-game-review.png'),fullPage:true});
});
