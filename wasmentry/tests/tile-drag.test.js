import {test, expect} from '@playwright/test';

const cell = (page, row, col) => page.locator(`#board [data-row="${row}"][data-col="${col}"]`);
const rackTile = (page, index) => page.locator('#rack-tiles .rack-tile').nth(index);
const rackLetters = page => page.locator('#rack-tiles .rack-tile').evaluateAll(tiles => tiles.map(tile => tile.dataset.letter).join(''));
async function point(locator, side = 'center') {
  const rect = await locator.boundingBox();
  return {x:side === 'before' ? rect.x + 1 : side === 'after' ? rect.x + rect.width - 1 : rect.x + rect.width / 2, y:rect.y + rect.height / 2};
}
async function drag(page, from, to, side = 'center', release = true) {
  await from.scrollIntoViewIfNeeded();
  const start = await point(from), end = await point(to, side);
  await page.mouse.move(start.x, start.y);
  await page.mouse.down();
  await page.mouse.move(end.x, end.y, {steps:10});
  if (release) await page.mouse.up();
}
async function ready(page, rack = null) {
  await page.setViewportSize({width:1440,height:1100});
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  if (rack == null) return;
  await page.locator('#new-game').click();
  await page.getByRole('button', {name:'Record a game',exact:true}).click();
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#status')).toHaveText('Game ready · enter the first rack');
  await page.locator('#rack').fill(rack);
  await page.locator('#rack').dispatchEvent('change');
  await page.evaluate(() => window.scrollTo(0,0));
}

test('rack insertion works before starting a game, with keyboard reordering too', async ({page}) => {
  await ready(page);
  await drag(page, rackTile(page, 0), page.locator('#rack-tiles'), 'after', false);
  await expect(page.locator('.rack-drop-caret')).toBeVisible();
  await expect(page.locator('#rack-tiles .drag-source')).toHaveCount(1);
  await expect(page.locator('.drag-tile sup')).toHaveText('1');
  await page.screenshot({path:test.info().outputPath('rack-insertion.png')});
  await page.mouse.up();
  expect(await rackLetters(page)).toBe('EINRSTA');
  await drag(page, rackTile(page, 6), rackTile(page, 0), 'before');
  expect(await rackLetters(page)).toBe('AEINRST');
  await rackTile(page, 1).focus();
  await page.keyboard.press('Alt+ArrowLeft');
  expect(await rackLetters(page)).toBe('EAINRST');
  await expect(rackTile(page, 0)).toBeFocused();
});

test('duplicate tiles retain rack order through all four drag paths; invalid drops are harmless', async ({page}) => {
  await ready(page, 'ABAC?');
  await drag(page, rackTile(page, 2), cell(page, 7, 7));
  expect(await rackLetters(page)).toBe('ABC?');
  await expect(cell(page, 7, 7)).toHaveClass(/pending/);
  await drag(page, cell(page, 7, 7), cell(page, 7, 8));
  await expect(cell(page, 7, 7)).not.toHaveClass(/played/);
  await expect(cell(page, 7, 8)).toHaveClass(/pending/);
  expect(await rackLetters(page)).toBe('ABC?');
  await drag(page, rackTile(page, 1), cell(page, 7, 8));
  expect(await rackLetters(page)).toBe('ABC?');
  await expect(page.locator('#board .pending')).toHaveCount(1);
  await drag(page, cell(page, 7, 8), page.locator('#rack-tiles'), 'after');
  expect(await rackLetters(page)).toBe('ABC?A');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await drag(page, rackTile(page, 4), rackTile(page, 1), 'before');
  expect(await rackLetters(page)).toBe('AABC?');
  // Release outside either surface.
  const start = await point(rackTile(page, 0));
  await page.mouse.move(start.x,start.y); await page.mouse.down();
  await page.mouse.move(5,200,{steps:8}); await page.mouse.up();
  expect(await rackLetters(page)).toBe('AABC?');
  await expect(page.locator('.drag-tile')).toHaveCount(0);
});

test('blanks cancel safely, keep their designation on the board, and return as question marks', async ({page}) => {
  await ready(page, 'AT?');
  await drag(page, rackTile(page, 2), cell(page, 7, 7));
  await expect(page.locator('#blank-dialog')).toBeVisible();
  await page.keyboard.press('Escape');
  expect(await rackLetters(page)).toBe('AT?');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await drag(page, rackTile(page, 2), cell(page, 7, 7));
  await page.getByRole('button',{name:'Blank as Z',exact:true}).click();
  await drag(page, cell(page, 7, 7), cell(page, 6, 7));
  await expect(cell(page, 6, 7)).toHaveClass(/blank/);
  await expect(cell(page, 6, 7).locator('.tile-letter')).toHaveText('Z');
  await expect(page.locator('#blank-dialog')).toBeHidden();
  await drag(page, cell(page, 6, 7), rackTile(page, 0), 'before');
  expect(await rackLetters(page)).toBe('?AT');
  await expect(rackTile(page, 0).locator('sup')).toHaveText('0');
});

for (const cancel of ['Escape', 'pointercancel', 'lostpointercapture', 'blur']) {
  test(`${cancel} cancels a drag without recalling other pending tiles`, async ({page}) => {
    await ready(page, 'AT?');
    await drag(page, rackTile(page, 0), cell(page, 7, 7));
    await page.evaluate(() => document.addEventListener('pointerdown', e => window.dragPointerId = e.pointerId, {once:true}));
    await drag(page, rackTile(page, 0), cell(page, 7, 8), 'center', false);
    await expect(page.locator('.drag-tile')).toHaveCount(1);
    if (cancel === 'Escape') await page.keyboard.press('Escape');
    else await page.evaluate(kind => {
      const rack = document.querySelector('#rack-tiles');
      if (kind === 'lostpointercapture') rack.releasePointerCapture(window.dragPointerId);
      else if (kind === 'blur') window.dispatchEvent(new Event('blur'));
      else rack.dispatchEvent(new PointerEvent('pointercancel', {pointerId:window.dragPointerId,bubbles:true}));
    }, cancel);
    await page.mouse.up();
    await expect(page.locator('.drag-tile')).toHaveCount(0);
    await expect(page.locator('#board .pending')).toHaveCount(1);
    expect(await rackLetters(page)).toBe('T?');
  });
}

test('committed tiles stay fixed in games; editor drags preserve ownership and undo', async ({page}) => {
  await ready(page, 'AT');
  await drag(page, rackTile(page, 0), cell(page, 7, 7));
  await drag(page, rackTile(page, 0), cell(page, 7, 8));
  // The empty rack remains a drop target.
  await drag(page, cell(page, 7, 8), page.locator('#rack-tiles'));
  expect(await rackLetters(page)).toBe('T');
  await drag(page, rackTile(page, 0), cell(page, 7, 8));
  await page.locator('#play-move').click();
  await expect(page.locator('#status')).toHaveText('Move recorded');
  await drag(page, cell(page, 7, 7), cell(page, 6, 7));
  await expect(cell(page, 7, 7)).toHaveClass(/played/);
  await expect(cell(page, 6, 7)).not.toHaveClass(/played/);
  await page.locator('#edit').check();
  await cell(page,7,7).click();
  await page.locator('#tile-owner').selectOption('1');
  await page.locator('#assign-owner').click();
  await expect(cell(page,7,7)).toHaveClass(/owner-1/);
  const owner = (await cell(page, 7, 7).getAttribute('class')).match(/owner-\d/)[0];
  await drag(page, cell(page, 7, 7), cell(page, 6, 7));
  await expect(cell(page, 6, 7)).toHaveClass(new RegExp(owner));
  await expect(cell(page, 7, 7)).not.toHaveClass(/played/);
  await page.locator('#undo').click();
  await expect(cell(page, 7, 7)).toHaveClass(new RegExp(owner));
  await expect(cell(page, 6, 7)).not.toHaveClass(/played/);
});

test('real touch input reorders, places, moves and returns a tile without scrolling the page', async ({page, browserName}) => {
  test.skip(browserName !== 'chromium', 'Playwright exposes touch movement via CDP only; mouse paths run in every engine.');
  await ready(page, 'AT?');
  await page.setViewportSize({width:390,height:844});
  await rackTile(page, 0).scrollIntoViewIfNeeded();
  const cdp = await page.context().newCDPSession(page);
  async function touch(from,to,side='center') {
    const start = await point(from), end = await point(to,side);
    await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[start]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[end]});
    await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
  }
  const scroll = await page.evaluate(() => scrollY);
  await touch(rackTile(page, 0),page.locator('#rack-tiles'),'after');
  expect(await rackLetters(page)).toBe('T?A');
  await touch(rackTile(page, 2),cell(page,14,7));
  await touch(cell(page,14,7),cell(page,13,7));
  await expect(cell(page,13,7)).toHaveClass(/pending/);
  await touch(cell(page,13,7),rackTile(page,0),'before');
  expect(await rackLetters(page)).toBe('AT?');
  expect(await page.evaluate(() => scrollY)).toBe(scroll);
});

test('history move editing can rearrange tiles without changing the recorded game until commit', async ({page}) => {
  await ready(page, 'AT?');
  await drag(page, rackTile(page, 0), cell(page,7,7));
  await drag(page, rackTile(page, 0), cell(page,7,8));
  await page.locator('#play-move').click();
  await expect(page.locator('#status')).toHaveText('Move recorded');
  await page.locator('#edit-history-move').click();
  await page.evaluate(() => window.scrollTo(0,0));
  await expect(page.locator('#board .pending')).toHaveCount(2);
  await drag(page, cell(page,7,8), cell(page,8,7));
  await expect(cell(page,8,7)).toHaveClass(/pending/);
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await page.locator('#play-move').click();
  await expect(page.locator('#game-confirm')).toBeVisible();
  await page.locator('#confirm-no').click();
  await expect(page.locator('#board .pending')).toHaveCount(2);
  await page.locator('#play-move').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#status')).toHaveText('Move recorded');
  await expect(cell(page,8,7)).toHaveClass(/played/);
  await expect(cell(page,7,8)).not.toHaveClass(/played/);
});

test('editor moves to and from the rack conserve tiles and undo; full racks reject returns', async ({page}) => {
  await ready(page, 'AT?');
  await page.locator('#edit').check();
  await page.evaluate(() => window.scrollTo(0,0));
  await drag(page, rackTile(page,0), cell(page,7,7));
  expect(await rackLetters(page)).toBe('T?');
  await expect(cell(page,7,7)).toHaveClass(/played/);
  await drag(page, cell(page,7,7), page.locator('#rack-tiles'),'after');
  expect(await rackLetters(page)).toBe('T?A');
  await expect(cell(page,7,7)).not.toHaveClass(/played/);
  await page.locator('#undo').click();
  await expect(cell(page,7,7)).toHaveClass(/played/);
  expect(await rackLetters(page)).toBe('T?');
  await page.locator('#rack').fill('AEINRST');
  await page.locator('#rack').dispatchEvent('change');
  await page.evaluate(() => window.scrollTo(0,0));
  await drag(page, cell(page,7,7), page.locator('#rack-tiles'));
  expect(await rackLetters(page)).toBe('AEINRST');
  await expect(cell(page,7,7)).toHaveClass(/played/);
});

test('starting analysis mid-drag cancels the gesture and leaves the rack intact', async ({page}) => {
  await ready(page, 'AEINRST');
  await drag(page, rackTile(page,0),cell(page,7,7),'center',false);
  await expect(page.locator('.drag-tile')).toHaveCount(1);
  await page.locator('[data-mode=kibitz]').evaluate(button => button.click());
  await expect(page.locator('.drag-tile')).toHaveCount(0);
  await page.mouse.up();
  await expect(page.locator('#status')).toHaveText('Complete');
  expect(await rackLetters(page)).toBe('AEINRST');
  await expect(page.locator('#board .pending')).toHaveCount(0);
});

test('touch dragging scrolls a short screen to reach the top of the board', async ({page, browserName}) => {
  test.skip(browserName !== 'chromium', 'Real touch movement requires CDP.');
  await ready(page, 'AT?');
  await page.setViewportSize({width:390,height:600});
  await rackTile(page,0).scrollIntoViewIfNeeded();
  const start = await point(rackTile(page,0));
  const initialScroll = await page.evaluate(() => scrollY);
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[start]});
  await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:start.x,y:8}]});
  await expect.poll(async () => (await cell(page,0,7).boundingBox()).y).toBeGreaterThan(55);
  const end = await point(cell(page,0,7));
  await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[end]});
  await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
  await expect(cell(page,0,7)).toHaveClass(/pending/);
  expect(await page.evaluate(() => scrollY)).toBeLessThan(initialScroll);
  expect(await rackLetters(page)).toBe('T?');
});
