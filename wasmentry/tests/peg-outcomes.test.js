import { test, expect } from '@playwright/test';
import { groupPegOutcomes, sortPegOutcomes } from '../peg-review.mjs';
import { analysisCommands, parseCGP, SAMPLES } from '../analysis-model.mjs';
import { openSample, loadSample } from './helpers/startup.js';

test('PEG grouping preserves ordered exceptions, multiplicities, margins and PVs', {tag:'@unit'}, () => {
  const rows = [
    {drawn:'BA', remaining:'CD', margin:10, weight:2},
    {drawn:'AB', remaining:'DC', margin:10, weight:2},
    {drawn:'A', remaining:'BCD', margin:-4, weight:3},
    {drawn:'A', remaining:'CBD', margin:-4, weight:3},
    {drawn:'A', remaining:'DCB', margin:0, weight:3},
    {drawn:'AA', remaining:'', margin:12, weight:1, pv:'H1 AT'},
    {drawn:'AA', remaining:'', margin:12, weight:1, pv:'H2 AT'},
  ];
  const grouped = groupPegOutcomes(rows);
  expect(grouped).toContainEqual({sequence:'AB/CD', margin:10, weight:4, pv:''});
  expect(grouped).toContainEqual({sequence:'A/BC/D', margin:-4, weight:6, pv:''});
  expect(grouped).toContainEqual({sequence:'A/D/C/B', margin:0, weight:3, pv:''});
  expect(grouped.filter(row => row.sequence === 'AA')).toHaveLength(2);
  for (const margin of [-4, 0, 10, 12])
    expect(grouped.filter(row => row.margin === margin).reduce((n, row) => n + row.weight, 0))
      .toBe(rows.filter(row => row.margin === margin).reduce((n, row) => n + row.weight, 0));
  expect(sortPegOutcomes(grouped, 'result')[0].margin).toBe(12);
  expect(sortPegOutcomes(grouped, 'result-asc')[0].margin).toBe(-4);
  expect(sortPegOutcomes(grouped, 'weight')[0].weight).toBe(6);
  const settings = {seconds:1, threads:2, candidates:50, plies:4, resumePeg:true};
  expect(analysisCommands(parseCGP(SAMPLES.peg), 'peg', settings).at(-1)).toMatch(/^peg resume .* -pegoutcomes true /);
  expect(analysisCommands(parseCGP(SAMPLES.peg), 'peg', {...settings, capturePegOutcomes:false}).at(-1)).toContain('-pegoutcomes false');
});

test('outcome table pages eight rows and sorts without losing principal variations', async ({page}) => {
  await page.goto('/wasmentry/');
  await page.evaluate(async () => {
    const {renderPegOutcomes} = await import('/wasmentry/peg-review.mjs');
    const cell = document.createElement('div');
    cell.id = 'outcome-fixture';
    document.body.replaceChildren(cell);
    renderPegOutcomes(cell, {depth:3, rows:Array.from({length:19}, (_, index) => ({
      drawn:String.fromCharCode(65 + index), remaining:'', margin:index - 9, weight:20 - index,
      pv:index === 18 ? '1A CAT → B2 DOG' : '',
    }))}, {page:0, sort:'result'});
  });
  await expect(page.locator('.peg-outcome-table tbody tr')).toHaveCount(8);
  await expect(page.locator('.peg-outcome-table tbody tr').first()).toContainText('Win 9');
  await page.locator('summary').click();
  await expect(page.locator('.peg-outcome-pv')).toBeVisible();
  await expect(page.locator('.peg-outcome-pv')).toContainText('1A CAT → B2 DOG');
  await page.getByRole('button', {name:'Next outcomes'}).click();
  await page.getByRole('button', {name:'Next outcomes'}).click();
  await expect(page.locator('.peg-outcome-table tbody tr')).toHaveCount(3);
  await expect(page.getByRole('button', {name:'Next outcomes'})).toBeDisabled();
  await page.getByLabel('Sort draw outcomes').selectOption('weight');
  await expect(page.locator('.peg-outcome-table tbody tr')).toHaveCount(8);
  await expect(page.locator('.peg-outcome-table tbody tr').first()).toContainText('Loss 9');
});

test('stopped PEG resumes in this session and exposes real weighted outcomes', async ({page, browserName}) => {
  test.skip(browserName !== 'chromium', 'Native checkpoint coverage plus one browser bridge integration.');
  test.setTimeout(90000);
  await page.addInitScript(() => {
    window.pegCommands = [];
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      if (message.type === 'run') message.data.commands = message.data.commands.map(command => {
        // An opponent's rack in the position is known to PEG; without it,
        // each of the bag's possible draws is its own outcome.
        if (command.startsWith('cgp ')) {
          const fields = command.split(' ');
          fields[2] = `${fields[2].split('/')[0]}/`;
          return fields.join(' ');
        }
        if (!command.startsWith('peg ')) return command;
        // The rational opponent keeps the resumed solve within the timeout.
        command = command.replace(/-pegtlim \S+/, '-pegtlim 0') + ' -pegtopk 16,8 -pegnested false -pegpess false';
        window.pegCommands.push(command);
        return command;
      });
      return post.call(this, message, ...args);
    };
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await openSample(page);
  await loadSample(page, 'peg');
  await page.locator('[data-mode=peg]').click();
  await expect(page.locator('#results tbody tr').first()).toBeVisible({timeout:15000});
  await page.locator('#stop').click();
  await expect(page.locator('body')).not.toHaveClass(/busy/, {timeout:15000});
  await expect(page.locator('[data-mode=peg]')).toHaveText('Resume preendgame');
  await page.locator('[data-mode=peg]').click();
  await expect(page.locator('#status')).toHaveText('Complete', {timeout:45000});
  expect((await page.evaluate(() => window.pegCommands)).at(-1)).toMatch(/^peg resume /);
  await page.locator('#results > tbody > tr').first().locator('button').click();
  await expect(page.locator('.peg-outcome-table tbody tr').first()).toBeVisible({timeout:10000});
  await expect(page.locator('.peg-outcome-view')).toContainText('3-ply');
  await expect(page.locator('.peg-outcome-table tbody tr')).toHaveCount(7);
  await page.screenshot({path:test.info().outputPath('peg-outcomes.png'), fullPage:true});
  // A tile placement empties this one-tile bag.
  await expect(page.locator('.peg-outcome-table summary').first()).toBeVisible();
  await page.locator('.peg-outcome-table summary').first().click();
  await expect(page.locator('.peg-outcome-pv').first()).toBeVisible();
  await expect(page.locator('.peg-outcome-pv').first()).not.toHaveText('');
  await page.locator('#fresh-analysis').click();
  await expect(page.locator('#status')).toHaveText('Complete', {timeout:45000});
  expect((await page.evaluate(() => window.pegCommands)).at(-1)).not.toMatch(/^peg resume /);
});
