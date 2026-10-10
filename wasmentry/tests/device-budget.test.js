import { test, expect } from '@playwright/test';
import { deviceBudget } from '../device-budget.mjs';
import { analysisCommands, parseCGP, SAMPLES } from '../analysis-model.mjs';

test('mobile hints choose bounded defaults without classifying narrow desktop windows', () => {
  expect(deviceBudget({userAgent:'Android', hardwareConcurrency:8, deviceMemory:4}))
    .toMatchObject({mobile:true, threads:2, maxThreads:4, ttMiB:16});
  expect(deviceBudget({userAgent:'iPhone', hardwareConcurrency:6}))
    .toMatchObject({mobile:true, threads:2, maxThreads:4, ttMiB:16});
  expect(deviceBudget({platform:'MacIntel', maxTouchPoints:5, hardwareConcurrency:8}))
    .toMatchObject({mobile:true, maxThreads:4});
  expect(deviceBudget({userAgentData:{mobile:true}, hardwareConcurrency:2}))
    .toMatchObject({mobile:true, threads:1, maxThreads:2});
  expect(deviceBudget({platform:'MacIntel', maxTouchPoints:0, hardwareConcurrency:16}))
    .toMatchObject({mobile:false, threads:4, maxThreads:16, ttMiB:32});
});

test('search commands propagate both TT budgets and reject invalid values', () => {
  for (const mode of ['peg', 'endgame']) {
    for (const ttMiB of [16,32]) {
      const commands=analysisCommands(parseCGP(SAMPLES[mode]),mode,{ttMiB,threads:4,seconds:1,plies:4,candidates:10});
      expect(commands[0]).toContain(`-ttfraction ${ttMiB/256}`);
    }
  }
  expect(()=>analysisCommands(parseCGP(SAMPLES.peg),'peg',{ttMiB:NaN})).toThrow('Search memory');
});

test('mobile settings default conservatively, persist memory choice, and keep WMP off', async ({page}) => {
  await page.addInitScript(()=>{
    Object.defineProperty(navigator,'userAgent',{get:()=> 'Android'});
    Object.defineProperty(navigator,'hardwareConcurrency',{get:()=>8});
    Object.defineProperty(navigator,'deviceMemory',{get:()=>4});
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await expect(page.locator('#threads')).toHaveValue('2');
  await expect(page.locator('#threads')).toHaveAttribute('max','4');
  await expect(page.locator('#tt-memory')).toHaveValue('16');
  await expect(page.locator('#wmp-source')).toHaveValue('off');
  await page.locator('#open-analysis-settings').click();
  await page.locator('#tt-memory').selectOption('32');
  await page.reload();
  await expect(page.locator('#tt-memory')).toHaveValue('32');
  await expect(page.locator('#wmp-source')).toHaveValue('off');
});

for (const mode of ['peg', 'endgame']) {
  test(`${mode} completes with the 16 MiB search budget`, async ({page}) => {
    await page.goto('/wasmentry/');
    await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
    await page.locator('#open-analysis-settings').click();
    await page.locator('#tt-memory').selectOption('16');
    await page.locator('#seconds').fill('2');
    await page.locator('#threads').fill('4');
    await page.locator('#back-to-game').click();
    await page.locator('#position-editor > summary').click();
    await page.selectOption('#sample',mode);
    await page.locator(`[data-mode=${mode}]`).click();
    await expect(page.locator('#status')).toHaveText('Complete',{timeout:30000});
    await expect(page.locator('#error')).toBeHidden();
    await expect(page.locator('#results tbody tr').first()).toBeVisible();
  });
}
