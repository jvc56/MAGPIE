import { openSample } from "./helpers/startup.js";
import {test, expect} from '@playwright/test';
import {analysisCommands, parseCGP, SAMPLES} from '../analysis-model.mjs';

test('postmortem solvers are untimed while computer turns and sims retain budgets', { tag: "@unit" }, () => {
  for (const mode of ['peg', 'endgame']) {
    const flag = mode === 'peg' ? '-pegtlim' : '-etlim';
    const settings = {seconds:10, threads:2, plies:4, candidates:10};
    const position = parseCGP(SAMPLES[mode]);
    expect(analysisCommands(position, mode, settings).join('\n')).toContain(`${flag} 10`);
    const unlimited = analysisCommands(position, mode, {...settings, solveToCompletion:true}).join('\n');
    expect(unlimited).toContain(`${flag} 0`);
    expect(unlimited).toContain('-tlim 0');
  }
  expect(analysisCommands(parseCGP(SAMPLES.midgame), 'sim', {seconds:10, threads:2, plies:4, candidates:10, solveToCompletion:true}).join('\n')).toContain('-tlim 10');
});

for (const mode of ['peg', 'endgame']) {
  test(`${mode} pauses a real search, resumes in place, and stops while paused`, async ({page}) => {
    test.setTimeout(90000);
    await page.addInitScript(() => {
      window.searchMessages = [];
      window.searchCommands = [];
      const post = Worker.prototype.postMessage;
      Worker.prototype.postMessage = function(message, ...args) {
        if (!this.observed) {
          this.observed = true;
          this.addEventListener('message', ({data}) => window.searchMessages.push(data));
        }
        if (message.type === 'run') window.searchCommands.push(...message.data.commands);
        return post.call(this, message, ...args);
      };
    });
    await page.goto('/wasmentry/');
    await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await openSample(page);
    await page.locator('#gcg-file').setInputFiles('tests/fixtures/review-candidates.gcg');
    await expect(page.locator('#status')).toContainText('Loaded');
    await page.selectOption('#history-position', mode === 'peg' ? '21' : '22');
    await page.locator('#open-analysis-settings').click();
    await page.locator('#seconds').fill('1');
    await page.locator('#threads').fill('2');
    await page.locator('#back-to-game').click();
    await expect(page.locator('#settings-summary')).toContainText('No time limit');
    await page.locator(`[data-mode=${mode}]`).click();
    await expect(page.locator('#pause-solve')).toBeVisible({timeout:30000});
    if (mode === 'peg') await expect(page.locator('#results tbody tr').first()).toBeVisible({timeout:15000});
    else await page.waitForTimeout(1500);
    await page.locator('#pause-solve').click();
    await expect(page.locator('#status')).toHaveText('Paused');
    await expect(page.locator('#pause-solve')).toHaveText('Resume');
    await page.locator('#open-analysis-settings').click();
    await expect(page.locator('#settings-busy')).toBeVisible();
    await expect(page.locator('#seconds')).toBeDisabled();
    await expect(page.locator('label[for="seconds"]')).toHaveCSS('opacity', '0.5');
    await page.locator('#back-to-game').click();
    // Allow in-flight work to reach its checkpoint, then check beyond the
    // old limit. Resume must keep the same native command and candidate set.
    await page.waitForTimeout(500);
    const rows = await page.locator('#results tbody').textContent();
    const elapsed = await page.locator('#elapsed').textContent();
    await page.waitForTimeout(1500);
    await expect(page.locator('#results tbody')).toHaveText(rows);
    await expect(page.locator('#elapsed')).toHaveText(elapsed);
    await page.locator('#pause-solve').click();
    await expect(page.locator('#pause-solve')).toHaveText('Pause');
    await expect(page.locator('#elapsed')).not.toHaveText(elapsed);
    await page.waitForTimeout(1500);
    await expect(page.locator('#status')).toContainText('Solving');
    await page.locator('#pause-solve').click();
    await expect(page.locator('#status')).toHaveText('Paused');
    await page.locator('#stop').click();
    await expect(page.locator('#status')).toHaveText('Stopped · partial results', {timeout:15000});
    await expect(page.locator('#pause-solve')).toBeHidden();
    await expect(page.locator('#seconds')).toBeEnabled();
    await expect(page.locator('#settings-busy')).toBeHidden();
    const commands = await page.evaluate(() => window.searchCommands);
    expect(commands.filter(command => command.startsWith(`${mode} `) || command === mode)).toHaveLength(1);
    expect(commands.join('\n')).toContain(mode === 'peg' ? '-pegtlim 0' : '-etlim 0');
    await page.locator('[data-mode=kibitz]').click();
    await expect(page.locator('#status')).toHaveText('Complete');
    await expect(page.locator('#error')).toBeHidden();
  });
}
