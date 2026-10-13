import { openSample } from "./helpers/startup.js";
import { test, expect } from "@playwright/test";
import { formatHistoryPlay, historyPlayParts, parseCGP, SAMPLES } from "../analysis-model.mjs";

test("history notation groups existing tiles and preserves blanks and special events", { tag: "@unit" }, () => {
  const position = parseCGP(SAMPLES.opening);
  position.board[7][4] = "A";
  expect(formatHistoryPlay(">p2: ABELNRW E7 N.RWAL +18 18", position)).toBe(
    "E7 N(A)RWAL +18 18",
  );
  position.board[7][5] = "t";
  expect(formatHistoryPlay(">p1: ER 8D E..R +4 28", position)).toBe(
    "8D E(At)R +4 28",
  );
  expect(formatHistoryPlay(">p1: AE - +0 24", position)).toBe("- +0 24");
  expect(formatHistoryPlay(">p1: AE -AE +0 24", position)).toBe("-AE +0 24");
  expect(formatHistoryPlay(">p1: AE -- -24 0", position)).toBe("-- -24 0");
});

test("history identifies end-rack and time adjustments without grouping passes or challenges", { tag: "@unit" }, () => {
  const position = parseCGP(SAMPLES.opening);
  for (const line of [">p1: (I) +2 378", ">p1: I (I) -1 376", ">p1: (time) -20 358"]) {
    expect(historyPlayParts(line, position).adjustment).toBe(true);
  }
  for (const line of [">p1: AE - +0 24", ">p1: AE -- -24 0", ">p1: AE (challenge) +5 29"]) {
    expect(historyPlayParts(line, position).adjustment).toBe(false);
  }
  expect(historyPlayParts(">p1: (I) +2 378", position)).toEqual({
    move: "(I)", score: "+2", cumulative: "378", adjustment: true,
  });
});

async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await openSample(page);
}

test("history pairs players and shows playthrough notation in rows and navigation", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await ready(page);
  const gcg =
    "#character-encoding UTF-8\n#lexicon CSW24\n#player1 p1 Player 1\n#player2 p2 Player 2\n>p1: AEFAULT 8D FAULT +24 24\n>p2: ABELNRW E7 N.RWAL +18 18\n>p1: AEINRST - +0 24\n";
  await page
    .locator("#gcg-file")
    .setInputFiles({
      name: "narwal.gcg",
      mimeType: "text/plain",
      buffer: Buffer.from(gcg),
    });
  await expect(page.locator("#history-table tbody tr")).toHaveCount(2);
  await expect(page.locator('#history-position option[value="0"]')).toHaveText('1. Player 1 · AAEFLTU');
  await expect(page.locator('#history-position option[value="1"]')).toHaveText('2. Player 2 · ABELNRW');
  await expect(page.locator('#history-position option[value="2"]')).toHaveText('3. Player 1 · AEINRST');
  await expect(page.locator('#history-position option[value="3"]')).toContainText('4. Player 2 ·');
  await page.selectOption("#history-position", "2");
  await expect(page.locator("#exchange-move + #challenge-move")).toHaveText("Challenge E7 N(A)RWAL");
  await expect(page.locator("#challenge-move")).toBeEnabled();
  const cells = page.locator("#history-table tbody tr").first().locator("td");
  await expect(page.locator("#history-table th")).toHaveText(["Player 1", "Player 2"]);
  await expect(cells).toHaveCount(2);
  await expect(cells.nth(0)).toContainText("8D FAULT");
  await expect(cells.nth(1)).toContainText("E7 N(A)RWAL");
  await expect(cells.nth(0).locator('.history-rack')).toHaveText('AAEFLTU');
  await expect(cells.nth(1).locator('.history-rack')).toHaveText('ABELNRW');
  await page.locator('#history-table [data-event-index="1"]').click();
  await expect(page.locator("#history-position")).toHaveValue("1");
  await expect(page.locator(".square.played")).toHaveCount(5);
  await expect(page.locator("#challenge-move")).toHaveText("Challenge 8D FAULT");
  await page.locator("#hide-spoilers").check();
  await expect(cells.nth(1)).toContainText("•••");
  await expect(cells.nth(1).locator(".history-rack")).toHaveText("ABELNRW");
  await expect(page.locator('#history-position option[value="2"]')).toHaveText('3. Player 1 · Rack hidden');
  await expect(page.locator('#history-position option[value="3"]')).toHaveText('4. Player 2 · Rack hidden');
  await page.locator("#hide-spoilers").uncheck();
  await page.locator("#history-first").click();
  await expect(page.locator("#challenge-move")).toBeHidden();
});

test("computer game history occupies the right panel without overlapping the board", async ({
  page,
}) => {
  await page.setViewportSize({ width: 1936, height: 890 });
  await ready(page);
  await page.locator("#open-settings").click();
  await page.selectOption("#theme", "light");
  await page.locator("#back-to-game").click();
  await page.locator("#new-game").click();
  await page.locator(".game-options > summary").click();
  await page.selectOption("#bot-analysis", "kibitz");
  await page.locator("#new-game-form button.primary").click();
  await page.locator("#pass-move").click();
  await page.locator("#confirm-yes").click();
  await expect(page.locator("#history-position option")).toHaveCount(3);
  await page.locator("#pause-game").click();
  for (const width of [1936, 1536, 1280]) {
    await page.setViewportSize({ width, height: 890 });
    await expect.poll(() => page.evaluate(() => {
      const board = document.querySelector(".position").getBoundingClientRect();
      const history = document.querySelector("#game-history").getBoundingClientRect();
      return {right: history.x >= board.right, aligned: history.y === board.y};
    })).toEqual({right:true, aligned:true});
  }
  await page.setViewportSize({ width: 1936, height: 890 });
  await page.evaluate(() => window.scrollTo(0, 0));
  await page.screenshot({
    path: test.info().outputPath("history-right-desktop.png"),
  });
  await page.locator("#show-live-analysis").check();
  const analysis = await page.locator(".analysis").boundingBox();
  const history = await page.locator("#game-history").boundingBox();
  expect(history.x).toBe(analysis.x);
  expect(history.y).toBeGreaterThanOrEqual(analysis.y + analysis.height);
  await page.setViewportSize({ width: 390, height: 844 });
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(
    390,
  );
  await page.locator("#show-live-analysis").uncheck();
  await page.screenshot({
    path: test.info().outputPath("history-right-mobile.png"),
    fullPage: true,
  });
});

test("final adjustments stay in the last turn and scores align in both player columns", async ({page}) => {
  await ready(page);
  await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#status')).toContainText('Loaded');
  const rows = page.locator('#history-table tbody tr');
  const events = await page.locator('#history-position option').count() - 1;
  const finalRow = rows.last();
  await expect(finalRow.locator(`button[data-event-index="${events - 3}"]`)).toBeVisible();
  await expect(finalRow.locator(`button[data-event-index="${events - 2}"] .history-score`)).toHaveText('+2');
  await expect(finalRow.locator(`button[data-event-index="${events - 1}"] .history-cumulative`)).toHaveText('358');
  await expect(finalRow.locator('button')).toHaveCount(4);
  const count = await rows.count();
  let turn = 1;
  for (let index = 0; index < events - 2; index++) {
    const label = await page.locator(`#history-position option[value="${index}"]`).textContent();
    if (label.startsWith("Challenge result")) {
      expect(label).toContain(` · ${turn - 1}. `);
    } else {
      expect(label.startsWith(`${turn}. `)).toBe(true);
      turn++;
    }
  }
  await expect(page.locator(`#history-position option[value="${events - 2}"]`)).toContainText('Final scoring');
  await expect(page.locator('#history-position option').last()).toHaveText('Game over');
  for (const width of [1440, 390]) {
    await page.setViewportSize({width, height:900});
    const alignment = await rows.first().locator('.history-score, .history-cumulative').evaluateAll(cells =>
      cells.map(cell => getComputedStyle(cell).textAlign));
    expect(alignment).toEqual(['right', 'right', 'right', 'right']);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  }
  await finalRow.locator(`button[data-event-index="${events - 2}"]`).click();
  await expect(page.locator('#history-position')).toHaveValue(String(events - 2));
  await expect(rows).toHaveCount(count);
  await page.locator('#hide-spoilers').check();
  await expect(finalRow.locator(`button[data-event-index="${events - 2}"]`)).toHaveText('•••');
  await page.locator('#hide-spoilers').uncheck();
  await page.setViewportSize({width:1440, height:900});
  await finalRow.scrollIntoViewIfNeeded();
  await page.locator('#game-history').screenshot({path:test.info().outputPath('history-aligned-scores.png')});
});


test("narrow desktop stacks panels and keeps history tile words on one line", async ({page}) => {
  await page.setViewportSize({width:1072, height:900});
  await ready(page);
  await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#status')).toContainText('Loaded');
  for (const width of [1000, 900, 390]) {
    await page.setViewportSize({width, height:900});
    await expect.poll(() => page.evaluate(() => {
      const board = document.querySelector('.position').getBoundingClientRect();
      const analysis = document.querySelector('.analysis').getBoundingClientRect();
      const history = document.querySelector('#game-history').getBoundingClientRect();
      return analysis.top >= board.bottom && history.top >= analysis.bottom && Math.abs(history.left - board.left) < 1;
    })).toBe(true);
    const wraps = await page.locator('#history-table .move-word').evaluateAll(words => words.some(word => {
      const tiles = [...word.querySelectorAll('.move-tile')];
      return tiles.some(tile => Math.abs(tile.getBoundingClientRect().top - tiles[0].getBoundingClientRect().top) > 1);
    }));
    expect(wraps).toBe(false);
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
  }
  await page.setViewportSize({width:1072, height:900});
  await page.locator('#game-history').screenshot({path:test.info().outputPath('unwrapped-history.png')});
});

test("history words use the full cell width and shrink long words without wrapping", async ({page}) => {
  await ready(page);
  await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#status')).toContainText('Loaded');
  await page.evaluate(async () => {
    const {moveTiles} = await import('/wasmentry/move-tiles.mjs');
    const play = document.querySelector('#history-table .history-play');
    play.classList.add('history-tile-play');
    play.querySelector('.move-tiles').replaceWith(moveTiles('8A ABCDEFGHIJKLMNO'));
  });
  for (const width of [1440, 1072, 900, 390]) {
    await page.setViewportSize({width, height:900});
    const metrics = await page.locator('#history-table button').first().evaluate(button => {
      const tiles = [...button.querySelectorAll('.move-tile')].map(tile => tile.getBoundingClientRect());
      const score = button.querySelector('.history-score').getBoundingClientRect();
      const cell = button.getBoundingClientRect();
      return {fits: tiles.at(-1).right <= cell.right, beneath:tiles[0].top >= score.bottom,
        square:Math.abs(tiles[0].width - tiles[0].height) < 1,
        single:tiles.every(tile => Math.abs(tile.top - tiles[0].top) < 1),
        width:tiles[0].width};
    });
    expect(metrics.fits).toBe(true);
    expect(metrics.beneath).toBe(true);
    expect(metrics.square).toBe(true);
    expect(metrics.single).toBe(true);
    if (width === 390) expect(metrics.width).toBeLessThan(18);
  }
});

test("time expiration keeps the next turn number in history navigation", async ({page}) => {
  await page.clock.install();
  await ready(page);
  await page.locator('#new-game').click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#game-time', '300');
  await page.selectOption('#overtime', '60');
  await page.locator('#new-game-form button.primary').click();
  const opening = await page.locator('#history-position option[value="0"]').textContent();
  await page.clock.fastForward(361000);
  await expect(page.locator('#session-result')).toContainText('lost on time');
  await expect(page.locator('#history-position option[value="0"]')).toHaveText(opening);
  await expect(page.locator('#history-position option[value="0"]')).toContainText('1. You ·');
});

test("compact header offers grouped file actions in Game overflow", async ({page}) => {
  await ready(page);
  await page.setViewportSize({width:1072,height:900});
  await expect(page.locator('#game-overflow')).toBeVisible();
  await expect(page.locator('#load-gcg')).toBeHidden();
  await page.locator('#game-overflow').click();
  await expect(page.locator('#game-file-actions [aria-label="GCG"]')).toBeVisible();
  await expect(page.locator('#game-file-actions [aria-label="CGP"]')).toBeVisible();
  await page.keyboard.press('Escape');
  await expect(page.locator('#load-gcg')).toBeHidden();
  await page.setViewportSize({width:1440,height:900});
  await expect(page.locator('#game-overflow')).toBeHidden();
  await expect(page.locator('#load-gcg')).toBeVisible();
  const board = await page.locator('.position').boundingBox();
  const analysis = await page.locator('.analysis').boundingBox();
  expect(analysis.x).toBeGreaterThanOrEqual(board.x + board.width);
});

test("single column fits the board and rack above the bottom of the viewport", async ({page}) => {
  await page.setViewportSize({width:900,height:844});
  await ready(page);
  await page.locator('#new-game').click();
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#pause-game')).toBeVisible();
  for (const width of [900, 700, 390]) {
    await page.setViewportSize({width,height:844});
    await expect.poll(() => page.locator('.rack-actions').evaluate(el => el.getBoundingClientRect().bottom)).toBeLessThan(815);
    await expect(page.locator('#rack-tiles')).toBeInViewport();
    await expect(page.locator('#board')).toBeInViewport();
  }
  await page.setViewportSize({width:900,height:844});
  await page.locator('#unseen-pool').evaluate(el => el.open = true);
  const tracker = await page.locator('#unseen-tiles').evaluate(el => {
    const groups = [...el.children];
    return {horizontal: groups[0].getBoundingClientRect().top === groups[1].getBoundingClientRect().top,
      intact: groups.every(group => getComputedStyle(group).whiteSpace === 'nowrap'),
      fits: el.scrollWidth <= el.clientWidth};
  });
  expect(tracker).toEqual({horizontal:true, intact:true, fits:true});
  const countTops = await page.locator('#bag-count, #unseen-label, #unseen-counts span').evaluateAll(elements => elements.map(el => el.getBoundingClientRect().top));
  expect(Math.max(...countTops) - Math.min(...countTops)).toBeLessThan(3);
  await page.screenshot({path:test.info().outputPath('board-rack-fit.png')});
});

test("board size stays stable through human and computer turn transitions", async ({page}) => {
  await page.setViewportSize({width:900,height:844});
  await ready(page);
  await page.locator('#new-game').click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#bot-analysis', 'kibitz');
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill('15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 BCDFGHJ/AEINRST 0/0 0');
  await page.locator('#new-game-form button.primary').click();
  // Starting a game loads the rack asynchronously; measure turn transitions
  // only after the initial human turn has rendered.
  await expect(page.locator('#session-result')).toHaveText('Your turn');
  await expect(page.locator('#session-result')).toBeHidden();
  await expect(page.locator('#name-one')).toHaveText('You · Your turn');
  // Invalid play feedback shares the controls row, and rack actions share the
  // move row when the single-column panel has room for all of the buttons.
  await page.locator('#play-move').click();
  await expect(page.locator('#error')).toBeVisible();
  const controls = await page.locator('#session-controls').boundingBox();
  const error = await page.locator('#error').boundingBox();
  expect(error.x).toBeGreaterThanOrEqual(controls.x + controls.width);
  expect(Math.abs((error.y + error.height / 2) - (controls.y + controls.height / 2))).toBeLessThan(2);
  const play = await page.locator('#play-move').boundingBox();
  const shuffle = await page.locator('#shuffle-rack').boundingBox();
  expect(Math.abs(play.y - shuffle.y)).toBeLessThan(2);
  await page.screenshot({path:test.info().outputPath('compact-game-controls.png')});
  for (const width of [900,1440]) {
    await page.setViewportSize({width,height:844});
    await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
    await page.evaluate(() => {
      window.boardSizes = [];
      window.boardSizeObserver = new ResizeObserver(entries => window.boardSizes.push(entries[0].contentRect.width));
      window.boardSizeObserver.observe(document.querySelector('#board'));
    });
    const count = await page.locator('#history-position option').count();
    await page.locator('#pass-move').click();
    await page.locator('#confirm-yes').click();
    await expect(page.locator('#history-position option')).toHaveCount(count + 2);
    await expect(page.locator('#pass-move')).toBeEnabled();
    const toast = page.locator('#computer-move');
    await expect(toast).toBeVisible();
    await expect(toast).toContainText('Magpie');
    const toastBox = await toast.boundingBox();
    const controlsBox = await page.locator('#session-controls').boundingBox();
    if (width === 900) {
      await expect(toast).toContainText('Magpie played');
      await expect(toast.locator('.move-tile').first()).toBeVisible();
      // Exercise the outlined blank's different baseline deterministically,
      // rather than relying on the computer's next randomly drawn rack.
      const blankHeight = await toast.evaluate(element => {
        const tile = element.querySelector('.move-tile');
        tile.classList.add('blank');
        const height = element.getBoundingClientRect().height;
        tile.classList.remove('blank');
        return height;
      });
      expect(Math.abs(blankHeight - toastBox.height)).toBeLessThan(0.1);
      expect(toastBox.x).toBeGreaterThanOrEqual(controlsBox.x + controlsBox.width);
      expect(Math.abs(toastBox.y + toastBox.height / 2 - controlsBox.y - controlsBox.height / 2)).toBeLessThan(2);
      await page.screenshot({path:test.info().outputPath('computer-move-toast.png')});
    }
    const sizes = await page.evaluate(() => {window.boardSizeObserver.disconnect(); return window.boardSizes;});
    expect(sizes.length).toBeGreaterThan(0);
    expect(Math.max(...sizes) - Math.min(...sizes)).toBeLessThan(1);
  }
  await expect(page.locator('#computer-move')).toBeHidden({timeout:15000});
});
