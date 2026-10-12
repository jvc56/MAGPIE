import { openSample } from "./helpers/startup.js";
import {test, expect} from '@playwright/test';

const cell = (page, row, col) => page.locator(`#board [data-row="${row}"][data-col="${col}"]`);
const rackTile = (page, index) => page.locator('#rack-tiles .rack-tile').nth(index);
const rackLetters = page => page.locator('#rack-tiles .rack-tile').evaluateAll(tiles => tiles.map(tile => tile.dataset.letter).join(''));
async function point(locator, side = 'center') {
  const rect = await locator.boundingBox();
  // Stay inside the target's leading/trailing half, clear of overlay scrollbars.
  const inset = Math.min(20, rect.width / 4);
  return {x:side === 'before' ? rect.x + inset : side === 'after' ? rect.x + rect.width - inset : rect.x + rect.width / 2, y:rect.y + rect.height / 2};
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
  await openSample(page);
  if (rack == null) return;
  await page.locator('#new-game').click();
  await page.getByRole('button', {name:'Review a game',exact:true}).click();
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#status')).toHaveText('Game ready · enter the first rack');
  await page.locator('#rack').fill(rack);
  await page.locator('#rack').dispatchEvent('change');
  // Finish editing before dragging; otherwise focus scrolls the board panel
  // back to the off-screen rack input during a pointer gesture.
  await page.locator('#position-editor > summary').click();
  await page.locator('.position').evaluate(panel => { panel.scrollTop = 0; });
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
  await drag(page, rackTile(page, 1), cell(page, 7, 8), 'center', false);
  // Invalid-move feedback arrives asynchronously while the pointer is held.
  // It must not resize the board and turn an occupied-square drop into a play.
  await expect(page.locator('#move-feedback')).not.toBeEmpty();
  await page.mouse.up();
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
  await page.locator('#position-editor > summary').click();
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
  await page.locator('#position-editor > summary').click();
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
  // Finish editing before dragging; otherwise focus scrolls the board panel
  // back to the off-screen rack input during a pointer gesture.
  await page.locator('#position-editor > summary').click();
  await page.locator('.position').evaluate(panel => { panel.scrollTop = 0; });
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
  await page.setViewportSize({width:390,height:400});
  await rackTile(page,0).scrollIntoViewIfNeeded();
  const start = await point(rackTile(page,0));
  const initialScroll = await page.evaluate(() => scrollY);
  expect(initialScroll).toBeGreaterThan(0);
  const cdp = await page.context().newCDPSession(page);
  await cdp.send('Input.dispatchTouchEvent',{type:'touchStart',touchPoints:[start]});
  await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[{x:start.x,y:8}]});
  // Let edge scrolling finish before taking the destination coordinates.
  await expect.poll(() => page.evaluate(() => scrollY)).toBe(0);
  const end = await point(cell(page,0,7));
  await cdp.send('Input.dispatchTouchEvent',{type:'touchMove',touchPoints:[end]});
  await cdp.send('Input.dispatchTouchEvent',{type:'touchEnd',touchPoints:[]});
  await expect(cell(page,0,7)).toHaveClass(/pending/);
  expect(await page.evaluate(() => scrollY)).toBeLessThan(initialScroll);
  expect(await rackLetters(page)).toBe('T?');
});

test('empty-square hover previews direction without moving the cursor or covering a drag', async ({page}) => {
  await ready(page, 'AEINRST');
  await expect(page.locator('#board-zoom')).toBeHidden();
  const center = cell(page, 7, 7), below = cell(page, 8, 7);
  const arrow = square => square.evaluate(element => {
    const style = getComputedStyle(element, '::after');
    return {content:style.content, transform:style.transform};
  });
  await center.hover();
  expect((await arrow(center)).content).toBe('""');
  await expect(center.locator('.tile-letter')).toHaveCSS('visibility', 'hidden');
  await expect(page.locator('#board .cursor')).toHaveCount(0);
  await center.click();
  await center.click();
  await below.hover();
  expect((await arrow(below)).content).toBe('none');
  await expect(center).toBeFocused();
  await page.keyboard.type('a');
  await cell(page, 9, 9).hover();
  expect((await arrow(cell(page, 9, 9))).content).toBe('none');
  await drag(page, center, cell(page, 8, 8), 'center', false);
  await expect(page.locator('.drag-tile')).toBeVisible();
  expect((await arrow(cell(page, 8, 8))).content).toBe('none');
  expect((await arrow(below)).content).toBe('none');
  await page.keyboard.press('Escape');
  await page.mouse.up();
  await expect(page.locator('.square.pending')).toHaveCount(1);
});

test('typing at a hovered square starts there without a click and then advances normally', async ({page}) => {
  await ready(page, 'AEINRT?');
  await page.locator('#sort-rack').focus();
  await cell(page, 7, 4).hover();
  await page.keyboard.type('at');
  await expect(cell(page, 7, 4).locator('.tile-letter')).toHaveText('A');
  await expect(cell(page, 7, 5).locator('.tile-letter')).toHaveText('T');
  await page.keyboard.press('Shift+Z');
  await expect(cell(page, 7, 6)).toHaveClass(/blank/);
  await page.keyboard.press('Backspace');
  await expect(cell(page, 7, 6)).not.toHaveClass(/pending/);
  await page.keyboard.press('Space');
  await expect(cell(page, 8, 4).locator('.tile-letter')).toHaveText('T');
  await cell(page, 9, 4).hover();
  await page.keyboard.type('in');
  await expect(cell(page, 9, 4).locator('.tile-letter')).toHaveText('I');
  await expect(cell(page, 10, 4).locator('.tile-letter')).toHaveText('N');
});

test('hover typing leaves text fields and open dialogs alone', async ({page}) => {
  await ready(page, 'AEINRST');
  await page.locator('#position-editor > summary').click();
  await page.locator('#rack').focus();
  await cell(page, 7, 7).hover();
  await page.keyboard.press('ControlOrMeta+A');
  await page.keyboard.type('AT');
  await expect(page.locator('#rack')).toHaveValue('AT');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await page.locator('#pass-move').click();
  await expect(page.locator('#game-confirm')).toBeVisible();
  await page.keyboard.type('a');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await page.keyboard.press('Escape');
});

test('unavailable typed tiles are silent and leave the draft and cursor unchanged', async ({page}) => {
  await ready(page, 'AT?');
  await cell(page, 7, 7).click();
  await page.keyboard.type('aq'); // Q uses the available blank.
  await expect(cell(page, 7, 8)).toHaveClass(/blank/);
  await expect(cell(page, 7, 9)).toHaveClass(/cursor/);
  await page.keyboard.type('x');
  await page.keyboard.press('Shift+T'); // An actual T cannot replace a requested blank.
  await expect(page.locator('#error')).toBeHidden();
  await expect(page.locator('#board .pending')).toHaveCount(2);
  await expect(cell(page, 7, 9)).toHaveClass(/cursor/);
  expect(await rackLetters(page)).toBe('T');
  await cell(page, 9, 9).hover();
  await page.keyboard.type('x');
  await expect(cell(page, 7, 9)).toHaveClass(/cursor/);
  await expect(page.locator('#error')).toBeHidden();
  // Clicking anchors entry; mouse movement cannot redirect a later valid tile.
  await page.keyboard.type('t');
  await expect(cell(page, 7, 9).locator('.tile-letter')).toHaveText('T');
  await page.keyboard.type('az');
  await expect(page.locator('#board .pending')).toHaveCount(3);
  await expect(cell(page, 7, 10)).toHaveClass(/cursor/);
  await expect(page.locator('#error')).toBeHidden();
});

for (const kind of ['watch', 'play']) test(`${kind}: visible rack can be arranged while the computer thinks, but cannot be played`, async ({page}) => {
  await ready(page);
  // Hold the search at its async boundary so the rack cannot change turns
  // halfway through a gesture on a fast test machine.
  await page.evaluate(async () => {
    const {EngineClient} = await import('/wasmentry/engine-client.mjs');
    const run = EngineClient.prototype.run;
    EngineClient.prototype.run = async function (...args) {
      window.searchHeld = true;
      window.emitSearchPreview = () => this.dispatchEvent(new CustomEvent("message", {detail:{
        type:"output", command:"generate", text:"1: 8H RETAINS 66 54.2"
      }}));
      await new Promise(resolve => { window.releaseSearch = resolve; });
      EngineClient.prototype.run = run;
      return run.apply(this, args);
    };
  });
  await page.locator('#new-game').click();
  await page.locator(`[data-game-kind="${kind}"]`).click();
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(`${Array(15).fill('15').join('/')} AEINRST/BCDFGHJ 0/0 0 -lex CSW24`);
  await page.locator('.game-options > summary').click();
  await page.selectOption('#game-time', '0');
  await page.selectOption('#bot-analysis', 'kibitz');
  if (kind === 'play') await page.selectOption('#human-side', '1');
  else await page.locator('#watch-delay').fill('0');
  await page.locator('#start-game').click();
  await expect.poll(() => page.evaluate(() => window.searchHeld)).toBe(true);
  await expect(page.locator('#session-result')).toHaveClass('thinking');
  await expect(page.locator('#session-result')).toBeHidden();
  await expect(page.locator('#name-one')).toContainText('is thinking…');
  await expect(page.locator('#name-one')).toHaveClass(/thinking/);
  await expect(page.locator('#name-one')).toHaveCSS('font-weight', '700');
  await expect(page.locator('#clock-one')).toHaveCSS('font-weight', '700');
  await expect(page.locator('#name-two')).toHaveCSS('font-weight', '400');
  await expect(page.locator('#clock-two')).toHaveCSS('font-weight', '400');
  await expect(page.locator('#points-one')).toHaveCSS('font-weight', '400');
  await expect(page.locator('#points-two')).toHaveCSS('font-weight', '400');
  await expect(page.locator('.player-scoreboard')).not.toContainText('On turn');
  const letters = kind === 'watch' ? 'AEINRST' : 'BCDFGHJ';
  expect(await rackLetters(page)).toBe(letters);
  await expect(page.locator('#play-move')).toBeDisabled();
  await cell(page, 7, 7).hover();
  await page.keyboard.type('a');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await drag(page, rackTile(page, 0), cell(page, 7, 7));
  expect(await rackLetters(page)).toBe(letters);
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await drag(page, rackTile(page, 0), rackTile(page, 6), 'after', false);
  await page.evaluate(() => {
    const square = document.querySelector('#board .square');
    window.emitSearchPreview();
    if (!square.isConnected) throw new Error('Search output replaced the board');
  });
  await expect(page.locator('.drag-tile')).toBeVisible();
  await page.mouse.up();
  expect(await rackLetters(page)).toBe(letters.slice(1) + letters[0]);
  await page.locator('#shuffle-rack').click();
  expect([...(await rackLetters(page))].sort().join('')).toBe(letters);
  await page.locator('#sort-rack').click();
  expect(await rackLetters(page)).toBe(letters);
  await rackTile(page, 0).hover();
  await expect(page.locator('#context-status')).not.toContainText('Alt');
  await expect(page.locator('#context-status')).not.toContainText('place');
  if (kind === 'play') await expect(page.locator('#opponent-rack')).toHaveValue('');
  await page.locator('#new-game').click();
  await expect(page.locator('#new-game-dialog')).toBeVisible();
  await expect(page.locator('#session-result')).not.toHaveClass('thinking');
  await expect(page.locator('#name-one')).not.toHaveClass(/thinking/);
  await expect(page.locator('#session-result')).toHaveText('Paused');
  await page.evaluate(() => window.releaseSearch());
  await expect(page.locator('body')).not.toHaveClass(/busy/);
  await expect(page.locator('#session-result')).toHaveText('Paused');
  await expect(page.locator('#start-game')).toBeEnabled();
  await page.locator('#cancel-new-game').click();
  await expect(page.locator('#pause-game')).toHaveText('Resume game');
  await cell(page, 7, 7).hover();
  await page.keyboard.type('a');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await expect(page.locator('#history-position option')).toHaveCount(1);
  await expect(page.locator('#error')).toBeHidden();
});

test('passing requires confirmation and Cancel or Escape preserve tentative tiles', async ({page}) => {
  await ready(page, 'AEINRST');
  await cell(page,7,7).click();
  await page.keyboard.type('at');
  const rack = await rackLetters(page);
  for (const cancel of ['button', 'escape']) {
    await page.locator('#pass-move').click();
    await expect(page.locator('#confirm-title')).toHaveText('Are you sure you wish to pass?');
    await expect(page.locator('#confirm-yes')).toHaveText('Pass');
    await expect(page.locator('#history-position option')).toHaveCount(1);
    if (cancel === 'button') await page.locator('#confirm-no').click();
    else await page.keyboard.press('Escape');
    await expect(page.locator('#game-confirm')).toBeHidden();
    await expect(page.locator('#board .pending')).toHaveCount(2);
    expect(await rackLetters(page)).toBe(rack);
    await expect(page.locator('#history-position option')).toHaveCount(1);
  }
  await page.locator('#pass-move').click();
  await page.locator('#confirm-yes').click();
  await expect(page.locator('#history-position option')).toHaveCount(2);
  await expect(page.locator('#history-table tbody')).toContainText('- +0 0');
  await expect(page.locator('#board .pending')).toHaveCount(0);
  await expect(page.locator('#error')).toBeHidden();
});


test.describe('mobile board entry', () => {
  test.use({hasTouch:true});

  test('double-tap requires a chosen start and advances through letters and a blank', async ({page}) => {
    await ready(page, 'AT?');
    await page.setViewportSize({width:390, height:844});
    const tapTwice = async tile => { await tile.tap(); await tile.tap(); };
    await tapTwice(rackTile(page, 0));
    await expect(page.locator('#board .pending')).toHaveCount(0);
    await cell(page, 7, 7).tap();
    await tapTwice(rackTile(page, 0));
    await expect(cell(page, 7, 7).locator('.tile-letter')).toHaveText('A');
    await expect(cell(page, 7, 8)).toHaveClass(/cursor/);
    await tapTwice(rackTile(page, 0));
    await expect(cell(page, 7, 8).locator('.tile-letter')).toHaveText('T');
    await tapTwice(rackTile(page, 0));
    await expect(page.locator('#blank-dialog')).toBeVisible();
    await page.getByRole('button', {name:'Blank as E',exact:true}).tap();
    await expect(cell(page, 7, 9)).toHaveClass(/blank/);
    await expect(cell(page, 7, 10)).toHaveClass(/cursor/);
    expect(await rackLetters(page)).toBe('');
    await page.locator('#recall-tiles').tap();
    await cell(page, 14, 14).tap(); await cell(page, 14, 14).tap();
    await tapTwice(rackTile(page, 0));
    await expect(cell(page, 14, 14)).toHaveClass(/pending/);
    await expect(page.locator('#board .edge-cursor')).toBeVisible();
    await tapTwice(rackTile(page, 0));
    await expect(page.locator('#board .pending')).toHaveCount(1);
    expect(await rackLetters(page)).toBe('T?');
  });

  test('zoom keeps rack outside a pannable board and can return to fit', async ({page, browserName}) => {
    await ready(page, 'AT');
    await page.setViewportSize({width:390,height:844});
    const zoom = page.locator('#board-zoom'), viewport = page.locator('#board-viewport');
    await expect(zoom).toBeVisible();
    await cell(page, 7, 7).tap();
    const before = await cell(page, 7, 7).boundingBox();
    await zoom.tap();
    await expect(zoom).toHaveAttribute('aria-pressed', 'true');
    const after = await cell(page, 7, 7).boundingBox();
    expect(after.width).toBeGreaterThan(before.width * 1.8);
    expect(await viewport.evaluate(el => el.scrollWidth > el.clientWidth && el.scrollHeight > el.clientHeight)).toBe(true);
    if (browserName === 'chromium') {
      const cdp = await page.context().newCDPSession(page);
      const bounds = await viewport.boundingBox();
      const beforeScroll = await viewport.evaluate(el => el.scrollLeft);
      const start = {x:bounds.x + bounds.width * .7, y:bounds.y + bounds.height * .4};
      await cdp.send('Input.dispatchTouchEvent', {type:'touchStart', touchPoints:[start]});
      for (let step = 1; step <= 8; step++) {
        await cdp.send('Input.dispatchTouchEvent', {type:'touchMove', touchPoints:[{x:start.x - step * 10, y:start.y}]});
        await page.evaluate(() => new Promise(requestAnimationFrame));
      }
      await cdp.send('Input.dispatchTouchEvent', {type:'touchEnd', touchPoints:[]});
      await expect.poll(() => viewport.evaluate(el => el.scrollLeft)).toBeGreaterThan(beforeScroll + 20);
      await cdp.detach();
    }
    await rackTile(page, 0).tap(); await rackTile(page, 0).tap();
    await expect(cell(page, 7, 7)).toHaveClass(/pending/);
    await page.screenshot({path:test.info().outputPath('mobile-zoom.png')});
    const rack = await page.locator('#rack-tiles').boundingBox();
    expect(rack.y + rack.height).toBeLessThan(844);
    await zoom.tap();
    await expect(zoom).toHaveAttribute('aria-pressed', 'false');
    expect(await viewport.evaluate(el => el.scrollLeft)).toBe(0);
  });
});
