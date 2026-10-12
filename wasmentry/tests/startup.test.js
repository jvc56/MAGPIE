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
  for (const name of ['Play Against Magpie', 'Review a game', 'Watch Magpie selfplay', 'Open game…'])
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

const startingCGP = '15/15/15/15/15/15/15/7A7/15/15/15/15/15/15/15 AEINRST/BCDFGHJ 100/80 1 -lex NWL23';

async function setStartingCGP(page) {
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(startingCGP);
}

for (const kind of ['play', 'watch', 'record']) test(`start ${kind} from CGP without earlier history`, async ({page}) => {
  await ready(page);
  await page.locator(`[data-game-kind=${kind}]`).click();
  await setStartingCGP(page);
  if (kind !== 'record') {
    await page.locator('.game-options > summary').click();
    await expect(page.locator('#first-player')).toBeDisabled();
    await page.locator('#bot-analysis').selectOption('kibitz');
    await page.locator('#game-time').selectOption('0');
    if (kind === 'watch') await page.locator('#watch-delay').fill('3');
  }
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option')).toHaveCount(1);
  await expect(page.locator('#history-position option')).toHaveText(/^1\. .* · AEINRST$/);
  await expect(page.locator('#board .played')).toHaveCount(1);
  await expect(page.locator('#open-lexicon-settings')).toContainText('NWL23');
  await expect(page.locator('#score')).toHaveValue('100');
  await expect(page.locator('#opponent-score')).toHaveValue('80');
  await expect(page.locator('#rack')).toHaveValue('AEINRST');
  if (kind === 'watch') {
    await expect(page.locator('#history-position option')).toHaveCount(2);
    await page.locator('#pause-game').click();
  } else {
    // Play through the initial A. This would fail on an empty starting board.
    await page.locator('#board [data-row="7"][data-col="8"]').click();
    await page.keyboard.type('t');
    await page.locator('#play-move').click();
    await expect(page.locator('#history-table tbody')).toContainText('(A)T +2 102');
  }
  await expect(page.locator('#error')).toBeHidden();
});

test('CGP review saves and reloads its starting board, scores, racks, and subsequent moves', async ({page}) => {
  const {readFile} = await import('node:fs/promises');
  await ready(page);
  await page.locator('[data-game-kind=record]').click();
  await setStartingCGP(page);
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option')).toHaveCount(1);
  await page.locator('#board [data-row="7"][data-col="8"]').click();
  await page.keyboard.type('t');
  await page.locator('#play-move').click();
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await page.locator('#history-first').click();
  await expect(page.locator('#board .played')).toHaveCount(1);
  await expect(page.locator('#score')).toHaveValue('100');
  await expect(page.locator('#rack')).toHaveValue('AEINRST');

  const download = page.waitForEvent('download');
  await page.locator('#save-gcg').click();
  const gcg = await readFile(await (await download).path(), 'utf8');
  expect(gcg).toContain('#cgp ' + startingCGP);
  expect(gcg.split('\n').filter(line => line.startsWith('>'))).toHaveLength(1);

  await page.locator('#load-gcg').click();
  await page.locator('#gcg-text').fill(gcg);
  await page.locator('#submit-gcg').click();
  await expect(page.locator('#gcg-dialog')).toBeHidden();
  await expect(page.locator('#history-position')).toHaveValue('0');
  await expect(page.locator('#board .played')).toHaveCount(1);
  await expect(page.locator('#score')).toHaveValue('100');
  await page.locator('#history-last').click();
  await expect(page.locator('#board .played')).toHaveCount(2);
  await expect(page.locator('#opponent-score')).toHaveValue('102');
});

test('CGP validates as entered and keeps invalid positions from starting a game', async ({page}) => {
  await ready(page);
  await setStartingCGP(page);
  for (const value of [
    'bad CGP',
    startingCGP.replace('7A7', '6A7'),
    startingCGP.replace('AEINRST', 'QQ'),
    startingCGP.replace('AEINRST', 'AEINRSTZ'),
    startingCGP.replace('NWL23', 'unknown'),
    startingCGP.replace('100/80 1', '100/80 6'),
  ]) {
    await page.locator('#new-game-cgp').fill(value);
    await expect(page.locator('#start-game')).toBeDisabled();
    await expect(page.locator('#new-game-cgp-status')).not.toBeEmpty();
    await expect(page.locator('#new-game-cgp')).toHaveAttribute('aria-invalid', 'true');
    await expect(page.locator('#new-game-dialog')).toBeVisible();
    await expect(page.locator('#board .played')).toHaveCount(0);
  }
  await page.locator('#new-game-cgp').fill(startingCGP);
  await expect(page.locator('#new-game-cgp-status')).toHaveText('Valid CGP · NWL23');
  await expect(page.locator('#new-game-cgp')).toHaveAttribute('aria-invalid', 'false');
  await expect(page.locator('#start-game')).toBeEnabled();
  await page.locator('#new-game-cgp').fill('');
  await expect(page.locator('#new-game-cgp-status')).toBeEmpty();
  await expect(page.locator('#start-game')).toBeEnabled();
  await page.locator('#start-game').click();
  await expect(page.locator('#play-session')).toBeVisible();
  await expect(page.locator('#board .played')).toHaveCount(0);
});

for (const theme of ['light', 'dark']) test(`expanded CGP help and validation fit desktop and mobile in ${theme}`, async ({page}) => {
  await ready(page);
  await page.evaluate(theme => { document.documentElement.dataset.theme = theme; }, theme);
  await expect(page.locator('#new-game-cgp-help')).toBeHidden();
  await setStartingCGP(page);
  await expect(page.locator('#new-game-cgp-help')).toContainText('scoreless-turns');
  await expect(page.locator('#new-game-cgp-help')).toContainText('lowercase letters');
  await expect(page.locator('#new-game-cgp-status')).toHaveText('Valid CGP · NWL23');
  for (const width of [1280, 390]) {
    await page.setViewportSize({width, height:844});
    await page.locator('#new-game-cgp').scrollIntoViewIfNeeded();
    await expect(page.locator('#new-game-cgp')).toBeInViewport();
    expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(width);
    expect(await page.locator('#new-game-dialog').evaluate(el => el.scrollWidth <= el.clientWidth)).toBe(true);
    await page.screenshot({path:test.info().outputPath(`cgp-${theme}-${width}.png`)});
    await page.locator('#start-game').scrollIntoViewIfNeeded();
    await expect(page.locator('#start-game')).toBeInViewport();
  }
});


test('New game pauses the human clock and cancel leaves it paused', async ({page}) => {
  await ready(page);
  await page.locator('#start-game').click();
  await expect(page.locator('#pause-game')).toHaveText('Pause');
  await page.locator('#new-game').click();
  await expect(page.locator('#new-game-dialog')).toBeVisible();
  await expect(page.locator('#session-result')).toHaveText('Paused');
  const clock = await page.locator('#clock-one').textContent();
  await page.waitForTimeout(1100);
  await expect(page.locator('#clock-one')).toHaveText(clock);
  await page.locator('#cancel-new-game').click();
  await expect(page.locator('#pause-game')).toHaveText('Resume game');
  await expect(page.getByRole('button', {name:'One turn',exact:true})).toHaveCount(0);
  await page.locator('#pause-game').click();
  await expect(page.locator('#session-result')).toHaveText('Your turn');
});
