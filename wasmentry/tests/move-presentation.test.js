import {test, expect} from '@playwright/test';
async function ready(page, review = true) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  if (review) await page.locator('[data-game-kind=record]').click();
}
async function record(page, rack) {
  await ready(page); await page.locator('#start-game').click();
  await page.locator('#rack').fill(rack); await page.locator('#rack').dispatchEvent('change');
  await page.locator('#board [data-row="7"][data-col="7"]').click();
}
test('adjacent draft score; Analyze Move adds and deduplicates; Play move accepts a preview', async ({page}) => {
  await record(page,'AEINRST'); await page.keyboard.type('retains');
  await expect(page.locator('.move-score')).toHaveText('66');
  await expect(page.locator('#move-feedback')).toBeEmpty();
  await expect(page.locator('#play-move + #analyze-move')).toBeVisible();
  await expect(page.locator('#add-sim')).toHaveCount(0);
  await page.locator('#analyze-move').click();
  await expect(page.locator('#status')).toHaveText('Play added');
  expect(await page.locator('#results tbody tr').count()).toBeGreaterThanOrEqual(50);
  await expect(page.locator('#results tbody button').filter({hasText:'8H RETAINS'})).toHaveCount(1);
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#stop')).toBeDisabled();
  const candidate=page.locator('#results tbody button').filter({hasText:'8H RETAINS'});
  await expect(candidate).toHaveCount(1); await candidate.click();
  await page.locator('#play-move').click();
  await expect(page.locator('#history-table .move-word')).toHaveCount(1);
  await expect(page.locator('#error')).toBeHidden();
  await expect(page.locator('#history-table .move-tile.owner-0')).toHaveCount(7);
});
test('withdrawn phony shares its turn cell; draft score gives no lexicon hint', async ({page}) => {
  await record(page,'II'); await page.keyboard.type('ii');
  await expect(page.locator('.move-score')).toHaveText('4');
  await expect(page.locator('#move-feedback')).toBeEmpty();
  await page.locator('#play-move').click(); await page.locator('#confirm-yes').click();
  await expect(page.locator('#challenge-move')).toBeEnabled(); await page.locator('#challenge-move').click();
  await expect(page.locator('#history-table tbody tr')).toHaveCount(1);
  const cell=page.locator('#history-table tbody td').first();
  await expect(cell.locator('button')).toHaveCount(2);
  await expect(cell).toContainText('Withdrawn after challenge');
  await expect(cell.locator('.history-rack')).toHaveCount(1);
  await expect(page.locator('#history-position option').last()).toContainText('2. Player 2');
});
test('uppercase blank tiles and gray playthrough work in both themes', async ({page}) => {
  await ready(page); await page.locator('#cancel-new-game').click();
  const gcg='#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n>A: ?EFAULT 8D FAuLT +22 22\n>B: ABELNRW E7 N.RWAL +18 18\n';
  await page.locator('#gcg-file').setInputFiles({name:'tiles.gcg',mimeType:'text/plain',buffer:Buffer.from(gcg)});
  await expect(page.locator('#history-table .move-tile.blank')).toHaveText('U');
  await expect(page.locator('#history-table .move-tile.playthrough')).toHaveText('A');
  for(const theme of ['dark','light']) {
    await page.locator('#open-settings').click(); await page.locator('#theme').selectOption(theme); await page.locator('#back-to-game').click();
    const colors=await page.locator('#history-table').evaluate(t=>['.owner-0','.owner-1','.playthrough'].map(s=>getComputedStyle(t.querySelector(s)).backgroundColor));
    expect(new Set(colors).size).toBe(3);
    await page.screenshot({path:test.info().outputPath(`move-tiles-${theme}.png`)});
  }
});
test('last successfully used time per player survives reload', async ({page}) => {
  await ready(page,false); await page.locator('#game-time').selectOption('300'); await page.locator('#start-game').click();
  await expect(page.locator('#status')).toHaveText('Game ready'); await page.reload();
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000}); await expect(page.locator('#game-time')).toHaveValue('300');
  await page.locator('#game-time').selectOption('900'); await page.locator('#cancel-new-game').click(); await page.reload();
  await expect(page.locator('#game-time')).toHaveValue('300');
});

test('named player selectors update as names change', async ({page}) => {
  await ready(page,false);
  await page.locator('#player-one-name').fill('Alice'); await page.locator('#player-two-name').fill('Magpie');
  await page.locator('.game-options > summary').click();
  await expect(page.getByLabel('You control')).toBeVisible();
  await expect(page.locator('#first-player option')).toHaveText(['Alice','Magpie','Random']);
  await expect(page.locator('#human-side option')).toHaveText(['Alice','Magpie']);
});
for (const down of [false,true]) test(`score stays before ${down?'vertical':'horizontal'} word at the board edge`, async ({page}) => {
  await ready(page);
  await page.locator('#new-game-position > summary').click();
  const rows = Array(15).fill('15');
  rows[down ? 1 : 0] = down ? 'T14' : '1T13';
  await page.locator('#new-game-cgp').fill(`${rows.join('/')} AEINRS/ 0/0 0`);
  await expect(page.locator('#start-game')).toBeEnabled();
  await page.locator('#start-game').click();
  await page.locator('#board [data-row="0"][data-col="0"]').click();
  if (down) await page.keyboard.press('Space');
  await page.keyboard.type('a');
  await expect(page.locator('.move-score')).toBeVisible();
  const marker=await page.locator('.move-score').evaluate(e=>[e.parentElement.dataset.row,e.parentElement.dataset.col]);
  expect(marker).toEqual(down ? ['-1','0'] : ['0','-1']);
  await page.keyboard.type('e');
  await expect(page.locator('.move-score')).toBeVisible();
  expect(await page.locator('.move-score').evaluate(e=>[e.parentElement.dataset.row,e.parentElement.dataset.col])).toEqual(marker);
  await page.screenshot({path:test.info().outputPath(`score-edge-${down?'vertical':'horizontal'}.png`)});
});
test('score circles fit one, two, and three digits at desktop and phone widths', async ({page}) => {
  await record(page,'AEINRST'); await page.keyboard.type('retains');
  await expect(page.locator('.move-score')).toHaveText('66');
  for (const width of [1440,390]) {
    await page.setViewportSize({width,height:900});
    const metrics = [];
    for (const score of ['4','66','125']) {
      metrics.push(await page.locator('.move-score').evaluate((badge,score) => {
        badge.textContent=score; badge.classList.toggle('move-score-wide',score.length>=3);
        const range=document.createRange(); range.selectNodeContents(badge);
        const text=range.getBoundingClientRect(), circle=badge.getBoundingClientRect();
        return {font:parseFloat(getComputedStyle(badge).fontSize), width:circle.width,height:circle.height, textWidth:text.width};
      },score));
      await page.locator('#board').screenshot({path:test.info().outputPath(`score-${width}-${score}.png`)});
    }
    expect(metrics[0].font).toBe(metrics[1].font);
    expect(metrics[2].font).toBeLessThan(metrics[1].font);
    for (const size of metrics) {
      expect(Math.abs(size.width-size.height)).toBeLessThan(1);
      expect(size.textWidth).toBeLessThan(size.width*0.8);
    }
  }
});
