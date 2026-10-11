import {test, expect} from '@playwright/test';

test('settings stay locked between computer searches and unlock after pausing', async ({page}) => {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await page.locator('#open-analysis-settings').click();
  await page.locator('#threads').fill('2');
  await page.locator('#plies').fill('1');
  await page.locator('#candidates').fill('2');
  await page.locator('#back-to-game').click();
  await page.locator('#new-game').click();
  await page.locator('[data-game-kind="watch"]').click();
  await page.locator('.game-options > summary').click();
  await page.locator('#bot-seconds').fill('1');
  await page.locator('#watch-delay').fill('1');
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#pause-game')).toBeVisible();
  await page.locator('#open-settings').click();
  await page.locator('[data-settings-section="analysis"]').click();
  await expect(page.locator('#settings-busy')).toHaveText('Pause the game to change these settings.');
  await expect(page.locator('#seconds')).toBeDisabled();
  await expect(page.locator('label[for="seconds"]')).toHaveCSS('opacity','0.5');
  await page.evaluate(() => {
    window.lockGaps=[];
    window.lockObserver=new MutationObserver(records => {
      for (const record of records) {
        // Detect a previously hidden notice or previously enabled input.
        if ((record.attributeName==='hidden' && record.oldValue!==null) ||
            (record.attributeName==='disabled' && record.oldValue===null)) {
          window.lockGaps.push(record.target.id+':'+record.attributeName);
        }
      }
    });
    window.lockObserver.observe(document.querySelector('#settings-busy'),{attributes:true,attributeOldValue:true,attributeFilter:['hidden']});
    window.lockObserver.observe(document.querySelector('#seconds'),{attributes:true,attributeOldValue:true,attributeFilter:['disabled']});
  });
  await expect.poll(() => page.locator('#history-position option').count(),{timeout:30000}).toBeGreaterThanOrEqual(5);
  expect(await page.evaluate(() => {window.lockObserver.disconnect();return window.lockGaps;})).toEqual([]);
  await page.locator('[data-settings-section="appearance"]').click();
  await expect(page.locator('#theme')).toBeEnabled();
  await expect(page.locator('label[for="theme"]')).toHaveCSS('opacity','1');
  await page.locator('#back-to-game').click();
  await page.locator('#pause-game').click();
  await expect(page.locator('#seconds')).toBeEnabled({timeout:30000});
  await page.locator('#open-settings').click();
  await page.locator('[data-settings-section="analysis"]').click();
  await expect(page.locator('#settings-busy')).toBeHidden();
  await expect(page.locator('label[for="seconds"]')).toHaveCSS('opacity','1');
  await page.locator('[data-settings-section="lexicon"]').click();
  await expect(page.locator('#lexicon')).toBeDisabled();
  await expect(page.locator('#lexicon-busy')).toHaveText('Finish the game to change the lexicon.');
  await expect(page.locator('label[for="lexicon"]')).toHaveCSS('opacity','0.5');
});
