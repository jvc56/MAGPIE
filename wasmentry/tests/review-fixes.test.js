import {test, expect} from '@playwright/test';
import {parseResults, SAMPLES} from '../analysis-model.mjs';
import {computerSearchPlan} from '../computer-time.mjs';

async function ready(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
}

test('appended played endgames are unranked and per-second overtime does not buy another minute', () => {
  const rows = parseResults('5K DA(WEN) 30 20 10\nplayed 13H (I)D 10 -3 -33', 'endgame');
  expect(rows.map(row => row.rank)).toEqual([1,null]);
  for (const remaining of [-1,-30]) expect(computerSearchPlan({time:180, remaining, overtime:'60', manageTime:true, seconds:10, penalty:'second'}, 20, 'sim').mode).toBe('kibitz');
});

test('computer exchange letters and analysis stay private during a live game', async ({page}) => {
  await ready(page);
  await page.locator('.game-options > summary').click();
  await page.locator('#human-side').selectOption('1');
  await page.locator('#bot-analysis').selectOption('kibitz');
  await page.locator('#game-time').selectOption('0');
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(`${Array(15).fill('15').join('/')} UUUVVWQ/ 0/0 0`);
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option')).toHaveCount(2,{timeout:30000});
  await expect(page.locator('#history-table .history-play').first()).toContainText(/-\d/);
  await expect(page.locator('#history-table')).not.toContainText('UUVVW');
  await expect(page.locator('#history-table .history-rack').first()).toHaveText('Rack hidden');
  await expect(page.locator('#history-position option').first()).toHaveText('1. Magpie · Rack hidden');
  await page.locator('#show-live-analysis').check();
  await expect(page.locator('#history-table .history-rack').first()).toHaveText('Rack hidden');
  await expect(page.locator('#opponent-rack')).toHaveValue('');
  await page.locator('#finish-game').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#history-table .history-rack').first()).toHaveText('QUUUVVW');
  await expect(page.locator('#history-position option').first()).toHaveText('1. Magpie · QUUUVVW');
  expect(await page.evaluate(() => localStorage.getItem('magpie-preview-recent') || '')).not.toContain('UUUVVWQ');
});

for (const rack of ['AEINRST', 'AEINRS?']) test(`a computer bingo reveals its historical rack (${rack}) but not its new rack`, async ({page}) => {
  await ready(page);
  await page.locator('.game-options > summary').click();
  await page.locator('#human-side').selectOption('1');
  await page.locator('#bot-analysis').selectOption('kibitz');
  await page.locator('#game-time').selectOption('0');
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(`${Array(15).fill('15').join('/')} ${rack}/ 0/0 0`);
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option')).toHaveCount(2, {timeout:30000});
  await expect(page.locator('#board .played')).toHaveCount(7);
  await expect(page.locator('#history-table .history-rack').first()).toHaveText(rack);
  await expect(page.locator('#history-position option').first()).toHaveText(`1. Magpie · ${rack}`);
  await expect(page.locator('#opponent-rack')).toHaveValue('');
  await page.locator('#history-first').click();
  // This preference can carry over from review, though its control is hidden during play.
  await page.locator('#hide-spoilers').evaluate(input => { input.checked = true; input.dispatchEvent(new Event('change')); });
  await expect(page.locator('#history-table .history-rack').first()).toHaveText('Rack hidden');
  await expect(page.locator('#history-position option').first()).toHaveText('1. Magpie · Rack hidden');
  await page.locator('#history-next').click();
  await expect(page.locator('#history-table .history-rack').first()).toHaveText(rack);
  await expect(page.locator('#opponent-rack')).toHaveValue('');
});

test('CGP unknown endgame rack is inferred and a new dialog clears the old start', async ({page}) => {
  await ready(page);
  await page.locator('[data-game-kind=record]').click();
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(SAMPLES.endgame.replace('FV/AADIZ','FV/'));
  await expect(page.locator('#start-game')).toBeEnabled();
  await page.locator('#start-game').click();
  await expect(page.locator('#rack')).toHaveValue('FV');
  await page.locator('#new-game').click();
  await expect(page.locator('#new-game-cgp')).toHaveValue('');
  await page.locator('#new-game-cgp').fill(SAMPLES.endgame.replace('FV/AADIZ','/AADIZFV'));
  await expect(page.locator('#start-game')).toBeDisabled();
  await expect(page.locator('#new-game-cgp-status')).toContainText('already ended');
});

test('cache write failures do not rebuild installed maps and a stopped setup does not build', async ({page}) => {
  await ready(page);
  const report = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const client = Object.setPrototypeOf(new EventTarget(), EngineClient.prototype);
    let builds = 0;
    client.run = async () => ({stopped:false});
    client.request = async () => { builds++; return {cached:false}; };
    await client.prepareWMP('CSW24','build',2,true);
    await client.prepareWMP('CSW24','build',2,true);
    await client.prepareWMP('CSW24','build',2,true);
    client.run = async () => ({stopped:true});
    await client.prepareWMP('NWL23','build',2,true);
    return {builds,key:client.wmpKey};
  });
  expect(report).toEqual({builds:1,key:null});
});

test('a failed computer search pauses once instead of retrying every second', async ({page}) => {
  await page.addInitScript(() => {
    window.failedSearches = 0;
    const send = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...rest) {
      if (message.type === 'run' && message.data.commands.includes('generate')) {
        window.failedSearches++;
        message = {...message, data:{...message.data, commands:['invalid_review_test_command']}};
      }
      return send.call(this, message, ...rest);
    };
  });
  await ready(page);
  await page.locator('.game-options > summary').click();
  await page.locator('#human-side').selectOption('1');
  await page.locator('#bot-analysis').selectOption('kibitz');
  await page.locator('#game-time').selectOption('0');
  await page.locator('#start-game').click();
  await expect(page.locator('#error')).not.toBeEmpty();
  await expect(page.locator('#pause-game')).toContainText('Resume');
  await page.waitForTimeout(2200);
  expect(await page.evaluate(() => window.failedSearches)).toBe(1);
});


test('reviewing a CGP endgame keeps the opponent rack after committing the first move', async ({page}) => {
  await ready(page);
  await page.locator('[data-game-kind=record]').click();
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(SAMPLES.endgame);
  await page.locator('#start-game').click();
  await expect(page.locator('#rack')).toHaveValue('FV');
  await page.locator('#board [data-row="5"][data-col="2"]').click();
  await page.keyboard.type('f');
  await page.locator('#play-move').click();
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await expect(page.locator('#rack')).toHaveValue('AADIZ');
  await expect(page.locator('#opponent-rack')).toHaveValue('V');
  await expect(page.locator('#error')).toBeHidden();
});
