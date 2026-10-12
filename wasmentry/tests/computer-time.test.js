import {test, expect} from '@playwright/test';
import {computerSearchPlan} from '../computer-time.mjs';
import {analysisCommands, parseCGP, SAMPLES} from '../analysis-model.mjs';

const clock = {time:300, remaining:300, overtime:'60', manageTime:true, seconds:5};
test('automatic budgets use the remaining clock and estimated plays instead of a fixed cap', () => {
  const opening = computerSearchPlan(clock, 100, 'sim');
  expect(opening.seconds).toBeGreaterThan(20);
  expect(opening.seconds).toBeLessThan(300 / 13);
  expect(computerSearchPlan(clock, 20, 'peg').seconds).toBeGreaterThan(opening.seconds);
  expect(computerSearchPlan({...clock, remaining:15}, 20, 'peg').seconds).toBeLessThan(5);
  expect(computerSearchPlan({...clock, remaining:1500}, 7, 'endgame').seconds).toBeGreaterThan(120);
  expect(computerSearchPlan({...clock, manageTime:false}, 100, 'sim').seconds).toBe(5);
  expect(computerSearchPlan({...clock, time:0}, 100, 'sim')).toMatchObject({seconds:5,useFullBudget:false});
});
test('short clocks and overtime boundaries use static play rather than a zero-second unlimited solve', () => {
  for (const mode of ['sim','peg','endgame']) {
    for (const override of [{remaining:0.1}, {remaining:0,overtime:'0'}, {remaining:-60}, {remaining:-90,overtime:'90'}])
      expect(computerSearchPlan({...clock,...override},20,mode).mode).toBe('kibitz');
    const overtime = computerSearchPlan({...clock,remaining:-61,overtime:'90'},14,mode);
    expect(overtime.seconds).toBeGreaterThan(10);
    expect(overtime.seconds).toBeLessThan(14.5);
    expect(computerSearchPlan({...clock,remaining:-61,overtime:'unlimited'},14,mode).seconds).toBeGreaterThan(overtime.seconds);
  }
});
test('automatic sims disable early stopping and review restores its normal policy', () => {
  const position = parseCGP(SAMPLES.opening);
  const settings = {seconds:23,threads:2,plies:1,candidates:3,useFullBudget:true};
  expect(analysisCommands(position,'sim',settings).join('\n')).toContain('-threshold none');
  expect(analysisCommands(position,'sim',{...settings,useFullBudget:false}).join('\n')).toContain('-threshold gk16');
});
async function setup(page) {
  await page.addInitScript(() => {
    window.searchCommands = [];
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      if (message.type === 'run') window.searchCommands.push(...message.data.commands);
      return post.call(this,message,...args);
    };
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  await page.locator('[data-game-kind=watch]').click();
  await page.locator('.game-options > summary').click();
}
test('automatic time defaults on; manual, untimed and static controls remain consistent', async ({page}) => {
  await setup(page);
  await expect(page.locator('#bot-auto-time')).toBeChecked();
  await expect(page.locator('#bot-seconds')).toBeDisabled();
  await page.locator('#bot-auto-time').uncheck();
  await expect(page.locator('#bot-seconds')).toBeEnabled();
  await page.locator('#bot-seconds').fill('7');
  await page.locator('#bot-auto-time').check();
  await page.locator('#game-time').selectOption('0');
  await expect(page.locator('#bot-auto-time')).toBeDisabled();
  await expect(page.locator('#bot-seconds')).toBeEnabled();
  await expect(page.locator('#bot-seconds')).toHaveValue('7');
  await page.locator('#game-time').selectOption('300');
  await expect(page.locator('#bot-auto-time')).toBeEnabled();
  await expect(page.locator('#bot-seconds')).toBeDisabled();
  await page.locator('#bot-analysis').selectOption('kibitz');
  await expect(page.locator('#bot-auto-time')).toBeDisabled();
  await expect(page.locator('#bot-seconds')).toBeDisabled();
  await page.setViewportSize({width:390,height:844});
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  await page.screenshot({path:test.info().outputPath('automatic-time-mobile.png'),fullPage:true});
});
for (const automatic of [true,false]) test(`${automatic ? 'automatic' : 'capped'} computer search uses its time policy and stays pauseable`, async ({page}) => {
  await setup(page);
  await page.locator('#bot-auto-time').uncheck();
  await page.locator('#bot-seconds').fill('1');
  await page.locator('#bot-auto-time').setChecked(automatic);
  await page.locator('#game-time').selectOption('180');
  await page.locator('#watch-delay').fill('0');
  await page.locator('#start-game').click();
  await expect.poll(() => page.evaluate(() => window.searchCommands.some(command => command.includes('-tlim')))).toBe(true);
  const command = await page.evaluate(() => window.searchCommands.find(command => command.includes('-tlim')));
  const seconds = Number(command.match(/-tlim ([\d.]+)/)[1]);
  if (automatic) {
    expect(seconds).toBeGreaterThan(10);
    expect(command).toContain('-threshold none');
    await expect(page.locator('#settings-summary')).toContainText('Automatic time management');
  } else {
    expect(seconds).toBe(1);
    expect(command).toContain('-threshold gk16');
  }
  await page.locator('#pause-game').click();
  await expect(page.locator('#stop')).toBeDisabled({timeout:10000});
  await expect(page.locator('#session-result')).toHaveText('Paused');
  await expect(page.locator('#error')).toBeHidden();
});

test('an automatic computer completes a real timed sim and records its move', async ({page}) => {
  await setup(page);
  await page.evaluate(() => {
    document.querySelector('#game-time').add(new Option('30 seconds (test)', '30'));
    window.completedSearches = [];
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message,...args) {
      if (message.type === 'run' && message.data.commands.some(command => command.includes('-threshold none'))) {
        const started = performance.now();
        const command = message.data.commands.find(command => command.includes('-threshold none'));
        const completed = ({data}) => {
          if (data.type !== 'complete' || data.requestId !== message.requestId) return;
          window.completedSearches.push({command,seconds:(performance.now()-started)/1000,stopped:data.stopped});
          this.removeEventListener('message',completed);
        };
        this.addEventListener('message',completed);
      }
      return post.call(this,message,...args);
    };
  });
  await page.locator('#game-time').selectOption('30');
  await page.locator('#watch-delay').fill('0');
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option').nth(1)).toBeAttached({timeout:20000});
  await page.locator('#pause-game').click();
  await expect(page.locator('#stop')).toBeDisabled();
  const run = await page.evaluate(() => window.completedSearches[0]);
  const budget = Number(run.command.match(/-tlim ([\d.]+)/)[1]);
  expect(budget).toBeGreaterThan(1);
  expect(run.stopped).toBe(false);
  expect(run.seconds).toBeGreaterThanOrEqual(budget * 0.9);
  expect(run.seconds).toBeLessThan(budget + 5);
  await expect(page.locator('#error')).toBeHidden();
});

test('Pause accepts a click held across live clock refreshes', async ({page}) => {
  await setup(page);
  await page.locator('#game-time').selectOption('180');
  await page.locator('#start-game').click();
  await expect.poll(() => page.evaluate(() => window.searchCommands.some(command => command.includes('-tlim')))).toBe(true);
  await page.locator('#pause-game').click({delay:800});
  await expect(page.locator('#session-result')).toHaveText('Paused');
  await expect(page.locator('#pause-game')).toHaveText('Resume game',{timeout:10000});
  await expect(page.locator('#error')).toBeHidden();
});
