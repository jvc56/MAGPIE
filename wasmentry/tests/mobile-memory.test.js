import { test, expect } from '@playwright/test';
import { openSample, loadSample } from './helpers/startup.js';
import { searchWordMapSource } from '../device-budget.mjs';

test('mobile late-game searches suspend word maps without changing earlier or desktop searches', {tag:'@unit'}, () => {
  for (const source of ['build', 'download', 'off']) {
    for (const bag of [0, 1, 4]) {
      expect(searchWordMapSource(source, bag, {mobile:true})).toBe('off');
      expect(searchWordMapSource(source, bag, {mobile:false})).toBe(source);
    }
    expect(searchWordMapSource(source, 5, {mobile:true})).toBe(source);
  }
});

test('releasing maps returns WASM allocations, preserves cached files, and allows late-game searches and reload', async ({page}) => {
  test.setTimeout(120000);
  await page.goto('/');
  const report = await page.evaluate(async () => {
    const {EngineClient} = await import('/wasmentry/engine-client.mjs');
    const {searchWordMapSource} = await import('/wasmentry/device-budget.mjs');
    const {parseCGP, analysisCommands, SAMPLES} = await import('/wasmentry/analysis-model.mjs');
    const {listWMPs} = await import('/wasmentry/wmp-cache.mjs');
    const engine = new EngineClient();
    const messages = [];
    engine.addEventListener('message', ({detail}) => messages.push(detail));
    try {
      await engine.prepare('CSW24');
      await engine.prepareWMP('CSW24', 'build', 2, true);
      const loaded = engine.memory;
      if (engine.wmpUnavailable) throw new Error('Maps were not installed');
      const settings = {threads:2,ttMiB:16,seconds:1,plies:2,candidates:10};
      const source = searchWordMapSource('build', 1, {mobile:true});
      await engine.prepareWMP('CSW24', source, 2, true);
      const released = engine.memory;
      const cached = await listWMPs();
      const board = '3J1OBE7/1DEAIRING6/1I1V11/MA1E11/IT1L11/nO8L4/UM3WAUK1O4/ES1R3PENNY3/T2E1YOOF1G4/EARLIES2ASH3/D2I3Z1AHA3/3C3O2iN3/3T3O2P4/AXE1REGIFTS4/WINCER1D7';
      await engine.run(analysisCommands(parseCGP(`${board} EIOQRUU/ADLNTTV 406/421 0 -lex CSW24`), 'peg', {...settings,wmp:false}));
      await engine.run(analysisCommands(parseCGP(SAMPLES.endgame), 'endgame', {...settings,wmp:false}));
      await engine.prepareWMP('CSW24', searchWordMapSource('build', 86, {mobile:true}), 2, true);
      const restored = engine.memory;
      await engine.run(analysisCommands(parseCGP(SAMPLES.opening), 'kibitz', {...settings,wmp:true}));
      return {loaded,released,restored,cached:cached.map(item=>item.lexicon),
        reloaded:messages.some(item=>item.type==='wmp_ready' && item.fromCache && item.witFromCache),
        outputs:messages.filter(item=>item.type==='output').map(item=>item.command)};
    } finally {engine.worker.terminate();}
  });
  expect(report.loaded.allocatedBytes - report.released.allocatedBytes).toBeGreaterThan(1024 * 1024);
  expect(report.released.freeBytes).toBeGreaterThan(report.loaded.freeBytes);
  expect(report.restored.allocatedBytes).toBeGreaterThan(report.released.allocatedBytes);
  expect(report.cached).toContain('CSW24');
  expect(report.reloaded).toBe(true);
  expect(report.outputs.some(command=>command.startsWith('peg '))).toBe(true);
  expect(report.outputs.some(command=>command.startsWith('endgame'))).toBe(true);
  expect(report.outputs).toContain('generate');
  for (const memory of [report.loaded, report.released, report.restored]) {
    expect(memory.allocatedBytes + memory.freeBytes).toBeLessThanOrEqual(memory.heapBytes);
    expect(memory.heapBytes).toBeLessThanOrEqual(memory.limitBytes);
  }
});


test('mobile analysis suspends the saved map preference for a late-game position', async ({page}) => {
  await page.addInitScript(() => {
    Object.defineProperty(navigator, 'userAgent', {get: () => 'iPhone'});
    localStorage.setItem('magpie-wmp-source', 'build');
    window.memoryTestMessages = [];
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      window.memoryTestMessages.push(structuredClone(message));
      if (message.type === 'run') {
        message.data.commands = message.data.commands.map(command => command.replace(/-pegtlim \S+/g, '-pegtlim 1'));
      }
      return post.call(this, message, ...args);
    };
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await openSample(page);
  await loadSample(page, 'peg');
  await page.locator('[data-mode="peg"]').click();
  await expect.poll(() => page.evaluate(() => window.memoryTestMessages
    .some(message => message.type === 'run' && message.data.commands.some(command => command.startsWith('peg '))))).toBe(true);
  await expect(page.locator('#stop')).toBeDisabled({timeout:30000});
  await expect(page.locator('#error')).toBeHidden();
  await expect(page.locator('#wmp-source')).toHaveValue('build');
  await expect(page.locator('#engine-memory')).toContainText('last idle snapshot');
  const messages = await page.evaluate(() => window.memoryTestMessages);
  expect(messages.some(message => message.type === 'prepare_wmp')).toBe(false);
  const commands = messages.filter(message => message.type === 'run').flatMap(message => message.data.commands);
  expect(commands).toContain('set -wit false -wmp false -lex NWL23');
  expect(commands.some(command => /-(wmp|wit) true\b/.test(command))).toBe(false);
});
