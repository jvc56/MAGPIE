import {test, expect} from '@playwright/test';
import {SAMPLES} from '../analysis-model.mjs';

const empty = Array(15).fill('15').join('/');
const header = '#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n';
async function ready(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
}
async function actions(page, input) {
  return page.evaluate(async input => {
    const {EngineClient} = await import('./engine-client.mjs');
    const engine = new EngineClient();
    await engine.prepare('CSW24');
    const reports = [];
    try {
      for (const data of input) {
        try {
          const {game} = await engine.gameAction({...data, lexicon:'CSW24'});
          const loaded = await engine.importGCG(game.gcg, 'CSW24');
          reports.push({game, reloaded:loaded.game});
        } catch(error) { reports.push({error:error.message}); }
      }
    } finally { engine.worker.terminate(); }
    return reports;
  }, input);
}

test('notes cannot inject another move or rack into the saved record', async ({page}) => {
  await ready(page);
  const report = await actions(page, ['a\n>B: BCDFGHJ - +0 0','a\n#rack2 BCDFGHJ','ordinary\nsecond line'].map(note => ({text:header+'>A: AEINRST 8H TRAIN +12 12\n',index:1,note,action:2})));
  expect(report[0].error).toContain('note cannot be saved');
  expect(report[1].error).toContain('note cannot be saved');
  expect(report[2].error).toBeUndefined();
  expect(report[2].reloaded.positions).toHaveLength(2);
});

test('native dealing reserves missing and partial opponent racks first', async ({page}) => {
  await ready(page);
  const report = await actions(page, ['FV/','FV/AD'].map(racks => ({text:header,cgp:SAMPLES.endgame.replace('FV/AADIZ',racks),action:4,onTurn:0,seed:42})));
  for (const result of report) {
    expect(result.error).toBeUndefined();
    expect(result.game.positions.at(-1).cgp).toContain(' FV/AADIZ 442/388 0');
  }
});

test('sixth scoreless live pass and exchange save the actual ending racks and scores', async ({page}) => {
  await ready(page);
  const passes = header + `#cgp ${empty} AEINRST/ 0/0 5\n`;
  const exchanges = header + '>A: AEINRST - +0 0\n>B: BCDFGHJ -BCD +0 0\n>A: AEINRST - +0 0\n>B: BCDFGHJ -BCD +0 0\n>A: AEINRST - +0 0\n';
  const report = await actions(page, [
    {text:passes,cgp:`${empty} AEINRST/DEILPUV 0/0 5`,rack:'AEINRST',move:'pass',action:5,index:0,onTurn:0},
    {text:exchanges,cgp:`${empty} AEINRST/BCDFGHJ 0/0 5`,rack:'BCDFGHJ',move:'ex BCD',action:5,index:5,onTurn:1,seed:42}
  ]);
  for (const result of report) {
    expect(result.error).toBeUndefined();
    expect(result.game.ended).toBe(true);
    expect(result.reloaded.positions.at(-1).cgp.split(" ").slice(2)).toEqual(result.game.positions.at(-1).cgp.split(" ").slice(2));
  }
  expect(report[0].game.gcg).toContain('(DEILPUV) -13');
});

test('reviewing a non-CGP endgame preserves the known opponent tiles', async ({page}) => {
  await ready(page);
  const report = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const {parseCGP,toCGP} = await import('./analysis-model.mjs');
    const engine = new EngineClient();
    try {
      await engine.prepare('CSW24');
      const text = await (await fetch('./tests/fixtures/standard.gcg')).text();
      const {game} = await engine.importGCG(text,'CSW24');
      const snap = game.positions[25];
      const pos = parseCGP(snap.cgp);
      if (snap.onTurn) { pos.racks.reverse(); pos.scores.reverse(); }
      const reply = await engine.gameAction({text,lexicon:'CSW24',index:25,rack:'EIU',move:'13M .EU',action:1,cgp:toCGP(pos),onTurn:snap.onTurn});
      const loaded = await engine.importGCG(reply.game.gcg,'CSW24');
      return {game:reply.game,loaded:loaded.game};
    } finally {engine.worker.terminate();}
  });
  expect(report.game.positions.at(-1).cgp).toContain(' OST/I ');
  expect(report.loaded.positions.at(-1).cgp).toBe(report.game.positions.at(-1).cgp);
});

test('a failed word-map build does not repeat after reload', async ({page}) => {
  await page.addInitScript(() => {
    if (!sessionStorage.getItem('seeded')) {
      localStorage.setItem('magpie-wmp-source','build');
      localStorage.setItem('magpie-wmp-building','CSW24');
      sessionStorage.setItem('seeded','yes');
    }
  });
  await ready(page);
  await expect(page.locator('#wmp-source')).toHaveValue('off');
  await expect(page.locator('#wmp-cache-status')).toContainText('did not finish');
  await page.reload();
  await expect(page.locator('#wmp-source')).toHaveValue('off');
  expect(await page.evaluate(() => localStorage.getItem('magpie-wmp-building'))).toBeNull();
});

test('adding a play never starts a word-map build', async ({page}) => {
  await page.addInitScript(() => {
    window.buildRequests = 0;
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message,...args) {
      if (message.type === 'prepare_wmp') window.buildRequests++;
      return post.call(this,message,...args);
    };
  });
  await ready(page);
  await page.locator('[data-game-kind=record]').click();
  await page.locator('#start-game').click();
  await page.locator('#rack').fill('AEINRST');
  await page.locator('#rack').dispatchEvent('change');
  await page.locator('#open-analysis-settings').click();
  await page.locator('#wmp-source').selectOption('build');
  await page.locator('#back-to-game').click();
  await page.locator('#board [data-row="7"][data-col="7"]').click();
  await page.keyboard.type('at');
  await page.locator('#add-sim').click();
  await expect(page.locator('#status')).toHaveText('Play added');
  expect(await page.evaluate(() => window.buildRequests)).toBe(0);
});

test('failed exports warn without disabling ordinary analysis', async ({page}) => {
  test.setTimeout(60000);
  await page.route('**/wasm-worker.js', async route => {
    const response = await route.fetch();
    await route.fulfill({response, body:(await response.text()).replace('const start = api.wmpData(), size = api.wmpDataSize();','const start = 0, size = 0;')});
  });
  await ready(page);
  const report = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const engine = new EngineClient(), warnings = [];
    engine.addEventListener('message', ({detail}) => {if(detail.type === 'wmp_cache_warning') warnings.push(detail.text)});
    try {
      await engine.prepare('CSW24');
      await engine.prepareWMP('CSW24','build',2,true);
      const response = await engine.run(['set -wmp false -wit false']);
      return {warnings, stopped:response.stopped, files:(await engine.wmpCache()).files};
    } finally {engine.worker.terminate();}
  });
  expect(report.warnings.join(' ')).toContain('Not enough memory to export WMP');
  expect(report.files).toEqual([]);
  expect(report.stopped).toBeFalsy();
});

test('computer analysis stays out of the DOM and history navigation does not regenerate it', async ({page}) => {
  await page.addInitScript(() => {
    window.searches = 0;
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message,...args) {
      if (message.type === 'run' && message.data.commands.includes('generate')) window.searches++;
      return post.call(this,message,...args);
    };
  });
  await ready(page);
  await page.locator('.game-options > summary').click();
  await page.locator('#human-side').selectOption('1');
  await page.locator('#first-player').selectOption('0');
  await page.locator('#bot-analysis').selectOption('kibitz');
  await page.locator('#game-time').selectOption('0');
  await page.locator('#start-game').click();
  await expect(page.locator('#history-position option')).toHaveCount(2,{timeout:30000});
  await page.locator('#open-settings').click();
  await page.locator('[data-settings-section=analysis]').click();
  await page.locator('#auto-analyze').selectOption('kibitz');
  await page.locator('#back-to-game').click();
  const count = await page.evaluate(() => window.searches);
  await page.locator('#history-first').click();
  await expect(page.locator('#results tbody tr')).toHaveCount(0);
  await expect(page.locator('#raw')).toBeEmpty();
  expect(await page.evaluate(() => window.searches)).toBe(count);
});

test('computer turns offer Pause instead of a misleading Stop analysis action', async ({page}) => {
  await ready(page);
  await page.locator('[data-game-kind=watch]').click();
  await page.locator('.game-options > summary').click();
  await page.locator('#watch-delay').fill('0');
  await page.locator('#bot-auto-time').uncheck();
  await page.locator('#bot-seconds').fill('10');
  await page.locator('#start-game').click();
  await expect(page.locator('#status')).toHaveText('Simulating…',{timeout:30000});
  await expect(page.locator('#stop')).toBeHidden();
  await page.locator('#pause-game').click();
  await expect(page.locator('#pause-game')).toContainText('Resume');
  await expect(page.locator('#error')).toBeEmpty();
});

test('a real constrained-heap build failure recovers after reload with WMP off', async ({page}) => {
  test.setTimeout(90000);
  await page.route('**/wasm-worker.js', async route => {
    const response = await route.fetch();
    const cap = `WebAssembly.Memory = new Proxy(WebAssembly.Memory, {construct(target,args){args[0].maximum = Math.min(args[0].maximum, 8192); return Reflect.construct(target,args);}});\n`;
    await route.fulfill({response,body:cap+await response.text()});
  });
  await ready(page);
  await page.locator('#cancel-new-game').click();
  await page.locator('#open-settings').click();
  await page.locator('[data-settings-section=analysis]').click();
  await page.locator('#wmp-source').selectOption('build');
  await page.getByRole('button',{name:'Cache CSW24 WMP',exact:true}).click();
  await expect(page.locator('#wmp-cache-status')).toContainText(/Reload|stopped/i,{timeout:60000});
  expect(await page.evaluate(() => localStorage.getItem('magpie-wmp-building'))).toBe('CSW24');
  await page.reload();
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  await expect(page.locator('#wmp-source')).toHaveValue('off');
  await page.locator('[data-game-kind=record]').click();
  await page.locator('#start-game').click();
  await page.locator('#rack').fill('AEINRST');
  await page.locator('#rack').dispatchEvent('change');
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Complete');
  await expect(page.locator('#results tbody tr').first()).toBeVisible();
});


test('review pass-out rejects unknown racks instead of inventing penalties', async ({page}) => {
  await ready(page);
  const report = await actions(page, ['', 'BC', 'BCDFGHJ'].map(opponent => {
    const cgp = `${empty} AEINRST/${opponent} 100/80 5`;
    return {text:header+`#cgp ${cgp}\n`,cgp,rack:'AEINRST',move:'pass',action:1,index:0,onTurn:0};
  }));
  for (const result of report.slice(0,2)) {
    expect(result.error).toContain('previous pass');
    expect(result.game).toBeUndefined();
  }
  expect(report[2].error).toBeUndefined();
  expect(report[2].game.ended).toBe(true);
  expect(report[2].reloaded.positions.at(-1).cgp).toContain('54/93');
});
