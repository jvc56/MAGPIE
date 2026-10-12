import { openGameActions } from "./helpers/startup.js";
import { openSample, loadSample } from "./helpers/startup.js";
import {test, expect} from '@playwright/test';
import {readFile} from 'node:fs/promises';
import {analysisCommands, parseCGP, SAMPLES, combineSim, endgameSummary, principalVariation} from '../analysis-model.mjs';

async function ready(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await openSample(page);
  await page.locator('#open-analysis-settings').click();
  await page.locator('#seconds').fill('1');
  await page.locator('#threads').fill('2');
  await page.locator('#candidates').fill('3');
  await page.locator('#plies').fill('2');
  await page.locator('#back-to-game').click();
}
async function complete(page) {
  await expect(page.locator('#stop')).toBeDisabled({timeout:60000});
  await expect(page.locator('#error')).toBeEmpty();
}
async function rows(page) {
  return page.locator('#results tbody tr:not(.endgame-details)').evaluateAll(rows => rows.map(row => ({move:row.querySelector('button').textContent, iters:Number(row.querySelector('[data-column=iterations]')?.textContent)})));
}

test('explicit candidate commands do not regenerate or silently add unselected plays', () => {
  const commands = analysisCommands(parseCGP(SAMPLES.opening), 'sim', {threads:2,seconds:1,plies:2,candidates:3,moves:['8H AT','pass'],playedMove:'8H RETAINS'});
  expect(commands).not.toContain('generate');
  expect(commands).toContain('addmoves 8H AT,pass');
  expect(commands.at(-1)).toBe('snoprune -,8H AT,pass');
  const row = {move:'14F ZI(N)E',iterations:100,win:50,equity:20,winSE:1,equitySE:1};
  expect(combineSim([row],[{...row,move:'14F ZI.E'}],parseCGP(SAMPLES.midgame))).toHaveLength(1);
  expect(combineSim([row],[{...row,move:'14F ZI.E'}],parseCGP(SAMPLES.midgame))[0].iterations).toBe(200);
  expect(endgameSummary('Endgame (depth: 25, time: 1s, status: finished)')).toBe('Fully searched');
  expect(endgameSummary('Endgame (depth: 25, time: 1s, status: interrupted)')).not.toBe('Fully searched');
  expect(principalVariation({move:'pass',score:0,continuation:'6B (E)F 10 pass 0'})).toEqual([{move:'pass',score:0},{move:'6B (E)F',score:10},{move:'pass',score:0}]);
  expect(principalVariation({move:'pass',score:0,continuation:'6B (E)F 10 pass 6A (ZE)D 16 pass 2B XI 9'})).toEqual([
    {move:'pass',score:0},{move:'6B (E)F',score:10},{move:'pass',score:0},
    {move:'6A (ZE)D',score:16},{move:'pass',score:0},{move:'2B XI',score:9},
  ]);
});

test('sims survive reload, accept a board play, and continue selected or all candidates', async ({page}) => {
  test.setTimeout(120000);
  await ready(page);
  await page.locator('#new-game').click();
  await page.getByRole('button',{name:'Review a game',exact:true}).click();
  await page.locator('#new-game-form button.primary').click();
  await page.locator('#rack').fill('AEINRST');
  await page.locator('#rack').dispatchEvent('change');
  await page.locator('#open-analysis-settings').click();
  await page.locator('#wmp-source').selectOption('build');
  await page.locator('#back-to-game').click();
  await page.locator('[data-mode=sim]').click(); await complete(page);
  const original = await rows(page);
  expect(original.length).toBe(3);
  await page.locator('#board [data-row="7"][data-col="7"]').click();
  await page.keyboard.type('at');
  await page.locator('#analyze-move').click();
  await complete(page);
  await expect(page.locator('#status')).toHaveText('Play added');
  const added = await rows(page);
  expect(added.length).toBe(4);
  expect(added.at(-1).iters).toBe(0);
  await page.locator('#continue-selected').click(); await complete(page);
  const selected = await rows(page);
  for (const old of original) expect(selected.find(row => row.move === old.move).iters).toBe(old.iters);
  expect(selected.find(row => row.move === added.at(-1).move).iters).toBeGreaterThan(0);
  await page.locator('[data-mode=sim]').click(); await complete(page);
  let all = await rows(page);
  for (const old of selected) expect(all.find(row => row.move === old.move).iters).toBeGreaterThan(old.iters);
  await expect(page.locator('#results input[type=checkbox]')).toHaveCount(0);
  const subset = [all[0].move];
  await page.locator('#results tbody [data-column=move] button').first().click();
  await page.locator('#continue-selected').click(); await complete(page);
  const subsetResults = await rows(page);
  for (const old of all) {
    const count = subsetResults.find(row => row.move === old.move).iters;
    if (subset.includes(old.move)) expect(count).toBeGreaterThan(old.iters);
    else expect(count).toBe(old.iters);
  }
  all = subsetResults;
  await expect.poll(() => page.evaluate(async () => {
    const {loadAnalyses} = await import('/wasmentry/analysis-cache.mjs');
    return [...(await loadAnalyses()).values()].some(saved => saved.results.length === 4 && saved.results.every(row => row.iterations > 500));
  })).toBe(true);
  await page.reload();
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await expect(page.locator('#new-game-dialog')).toBeVisible();
  await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(0);
  await openSample(page);
  expect(await rows(page)).toEqual(all);
  await page.screenshot({path:test.info().outputPath('saved-sim.png')});
});

for (const kind of ['play','watch']) test(`${kind} computer sims reappear in review and after GCG reimport`, async ({page}) => {
  test.setTimeout(120000);
  await ready(page);
  await page.locator('#new-game').click();
  await page.locator(`[data-game-kind="${kind}"]`).click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#first-player','0');
  await page.selectOption('#game-time','0');
  await page.locator('#bot-seconds').fill('1');
  if (kind === 'watch') await page.locator('#watch-delay').fill('2');
  await page.locator('#new-game-form button.primary').click();
  if (kind === 'play') {
    await page.locator('#pass-move').click();
    await page.locator('#confirm-yes').click();
  }
  await expect(page.locator('#history-position option')).toHaveCount(kind === 'play' ? 3 : 2,{timeout:60000});
  await complete(page);
  if (kind === 'watch') await page.locator('#pause-game').click();
  await page.locator('#finish-game').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#finish-game')).toBeDisabled();
  await page.selectOption('#history-position',kind === 'play' ? '1' : '0');
  await expect(page.locator('#results tbody tr').first()).toBeVisible();
  const saved = await rows(page);
  expect(saved.every(row => row.iters > 0)).toBe(true);
  await expect(page.locator('[data-mode=sim]')).toHaveText('Continue sim');
  await page.locator('[data-mode=sim]').click(); await complete(page);
  const continued = await rows(page);
  for (const old of saved) expect(continued.find(row => row.move === old.move).iters).toBeGreaterThan(old.iters);

  const downloadEvent = page.waitForEvent('download');
  await openGameActions(page);
  await page.locator('#save-gcg').click();
  const download = await downloadEvent;
  const gcg = await readFile(await download.path());
  await page.reload();
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await openSample(page);
  await page.locator('#gcg-file').setInputFiles({name:'saved.gcg',mimeType:'text/plain',buffer:gcg});
  await expect(page.locator('#game-history')).toBeVisible();
  if (kind === 'play') await page.locator('#history-next').click();
  expect(await rows(page)).toEqual(continued);
});

test('endgame rows expand in place, preview the colored PV at half opacity, and combine outcomes', async ({page}) => {
  test.setTimeout(90000);
  await ready(page);
  await page.locator('#position-editor > summary').click();
  await loadSample(page, 'endgame');
  await page.locator('[data-mode=endgame]').click(); await complete(page);
  // A time-limited search can finish at a shallower depth on CI.
  // The exact full-search label is covered by the model tests.
  await expect(page.locator('#analysis-meta')).toHaveText(/Fully searched|\d+ plies · finished/);
  await expect(page.locator('#results thead')).not.toContainText('Spread');
  await expect(page.locator('#results tbody tr').first()).toContainText(/(?:Win|Loss) by \d+|Tie/);
  const first = page.locator('#results .endgame-expand').first();
  await first.click();
  await expect(first).toHaveAttribute('aria-expanded','true');
  await expect(page.locator('#results .endgame-details')).toContainText('Principal variation');
  await expect(page.locator('#selection')).toBeHidden();
  const previews = await page.locator('.pv-preview').evaluateAll(tiles => tiles.map(tile => ({opacity:getComputedStyle(tile).opacity,owner:tile.classList.contains('owner-1') ? 1 : 0})));
  expect(previews.length).toBeGreaterThan(0);
  expect(previews.every(tile=>tile.opacity === '0.5')).toBe(true);
  expect(new Set(previews.map(tile=>tile.owner)).size).toBe(2);
  await page.screenshot({path:test.info().outputPath('endgame-expanded.png')});
  await page.locator('.pv-line button').first().click();
  expect(await page.locator('.pv-preview').count()).toBeLessThan(previews.length);
  await first.click();
  await expect(page.locator('.endgame-details')).toHaveCount(0);
  await expect(page.locator('.pv-preview')).toHaveCount(0);
});

test('unavailable browser storage preserves usable in-tab simulations', async ({page}) => {
  await page.addInitScript(() => {
    const open = indexedDB.open.bind(indexedDB);
    indexedDB.open = (name,...args) => {
      if (name === 'magpie-analysis-v1') throw new DOMException('Storage unavailable','SecurityError');
      return open(name,...args);
    };
  });
  await ready(page);
  await expect(page.locator('#analysis-storage-warning')).toBeVisible();
  await page.locator('[data-mode=sim]').click(); await complete(page);
  const saved = await rows(page);
  await page.locator('[data-mode=sim]').click(); await complete(page);
  const continued = await rows(page);
  for (const old of saved) expect(continued.find(row => row.move === old.move).iters).toBeGreaterThan(old.iters);
});
