import { test, expect } from '@playwright/test';

for (const submit of ['Enter', 'button']) {
  for (const col of [7, 8]) {
    test(`rejected phony stays keyboard editable after ${submit}, starting column ${col}`, async ({page}) => {
      await page.goto('/wasmentry/');
      await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
      await page.locator('#new-game-position > summary').click();
      const rows = Array(15).fill('15');
      if (col === 8) rows[7] = '7A7';
      await page.locator('#new-game-cgp').fill(
        `${rows.join('/')} ${col === 7 ? '?AEINRS' : 'AEINRST'}/BCDFGHJ 0/0 0 -lex CSW24`,
      );
      await page.locator('.game-options > summary').click();
      await page.locator('#game-time').selectOption('0');
      await page.locator('#challenge-rule').selectOption('void');
      await page.locator('#start-game').click();
      await expect(page.locator('#new-game-dialog')).toBeHidden();
      await page.locator(`[data-row="7"][data-col="${col}"]`).click();
      if (col === 7) await page.keyboard.press('Shift+T');
      else await page.keyboard.type('t');
      await page.keyboard.type('sraine');
      await expect(page.locator('.square.pending')).toHaveCount(7);
      if (submit === 'Enter') await page.keyboard.press('Enter');
      else await page.locator('#play-move').click();
      const phonyMessage = `Play 8H ${col === 8 ? '(A)' : ''}TSRAINE contains at least one phony word`;
      await expect(page.locator('#error')).toHaveText(phonyMessage);
      await expect(page.locator('#error .move-tile')).toHaveCount(col === 8 ? 8 : 7);
      await expect(page.locator('#error .move-coordinate')).toHaveText('8H');
      if (col === 7) {
        await expect(page.locator('#error .move-tile.blank')).toHaveText('T');
        const tops = await page.locator('#error .move-tile').evaluateAll(tiles =>
          tiles.map(tile => tile.getBoundingClientRect().top));
        expect(Math.max(...tops) - Math.min(...tops)).toBeLessThan(0.1);
      }
      const gaps = await page.locator('#error').evaluate(error => {
        const coordinate=error.querySelector('.move-coordinate').getBoundingClientRect();
        const tiles=error.querySelector('.move-word').getBoundingClientRect();
        const trailing=error.querySelector('.move-message-gap').getBoundingClientRect();
        return [tiles.left-coordinate.right,trailing.width];
      });
      expect(Math.abs(gaps[0]-gaps[1])).toBeLessThan(1);
      await expect(page.locator('#play-move')).toBeEnabled();
      await page.keyboard.press('Backspace');
      await expect(page.locator('.square.pending')).toHaveCount(6);
      await expect(page.locator('#rack-tiles [data-letter="E"]')).toHaveCount(1);
      await expect(page.locator('#error')).toBeHidden();
      await expect(page.locator('#history-position option')).toHaveCount(1);
      await page.keyboard.type('e');
      await expect(page.locator('.square.pending')).toHaveCount(7);
      await page.keyboard.press('Enter');
      await expect(page.locator('#error')).toHaveText(phonyMessage);
      if (submit === 'Enter') await page.keyboard.press('Escape');
      else await page.locator('#sort-rack').click();
      await expect(page.locator('.square.pending')).toHaveCount(0);
      await expect(page.locator('#rack-tiles .rack-tile')).toHaveCount(7);
      await expect(page.locator('#error')).toBeHidden();
      await expect(page.locator('#move-feedback')).toBeEmpty();
    });
  }
}

for (const down of [false,true]) test(`Backspace continues ${down?'up':'left'} past the initial typing square`, async ({page}) => {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  await page.locator('[data-game-kind=record]').click();
  await page.locator('#start-game').click();
  await page.locator('#rack').fill('AEINRST'); await page.locator('#rack').dispatchEvent('change');
  await page.locator('#board [data-row="7"][data-col="7"]').click();
  if (down) await page.keyboard.press('Space');
  await page.keyboard.type('a');
  await page.keyboard.press('Backspace');
  await expect(page.locator('.square.pending')).toHaveCount(0);
  await page.keyboard.press('Backspace');
  await expect(page.locator('.square.cursor')).toHaveAttribute(down?'data-row':'data-col','6');
  await page.keyboard.type('a');
  await expect(page.locator('.square.pending')).toHaveAttribute(down?'data-row':'data-col','6');
  await page.keyboard.press('Backspace');
  for (let index=0;index<8;index++) await page.keyboard.press('Backspace');
  await expect(page.locator('.square.cursor')).toHaveAttribute(down?'data-row':'data-col','0');
});

for (const down of [false, true]) test(`typing to the ${down?'bottom':'right'} edge keeps an off-board arrow`, async ({page}) => {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  await page.locator('[data-game-kind=record]').click(); await page.locator('#start-game').click();
  await page.locator('#rack').fill('AEINRST'); await page.locator('#rack').dispatchEvent('change');
  await page.locator(`#board [data-row="${down?14:7}"][data-col="${down?7:14}"]`).click();
  if (down) await page.keyboard.press('Space');
  await page.keyboard.type('a');
  await expect(page.locator('.edge-cursor')).toBeVisible();
  await expect(page.locator('.edge-cursor')).toBeFocused();
  await expect(page.locator('.edge-cursor')).toHaveAttribute(down?'data-row':'data-col','15');
  await page.keyboard.type('e');
  await expect(page.locator('.square.pending')).toHaveCount(1);
  for (const width of [1936,390]) {
    await page.setViewportSize({width,height:900});
    await expect(page.locator('.edge-cursor')).toBeVisible();
    expect(await page.evaluate(()=>document.documentElement.scrollWidth)).toBe(width);
    await page.screenshot({path:test.info().outputPath(`edge-${down}-${width}.png`)});
  }
  await page.keyboard.press('Backspace');
  await expect(page.locator('.edge-cursor')).toHaveCount(0);
  await expect(page.locator('.square.pending')).toHaveCount(0);
  await expect(page.locator('.square.cursor')).toHaveAttribute(down?'data-row':'data-col','14');
});
