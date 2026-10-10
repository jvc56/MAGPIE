const { test, expect } = require('@playwright/test');

test('local and downloaded WMPs persist, match ordinary move generation, and can be removed', async ({page}) => {
  test.setTimeout(120000);
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  const report = await page.evaluate(async () => {
    const { EngineClient } = await import('./engine-client.mjs');
    const { listWMPs } = await import('./wmp-cache.mjs');
    const messages = [];
    let engine;
    const create = () => {
      engine = new EngineClient();
      engine.addEventListener('message', ({detail}) => messages.push(detail));
    };
    const generate = async (lexicon, wmp, cgp) => {
      await engine.run([`set -lex ${lexicon} -wmp ${wmp} -wit ${wmp} -numplays 100 -hr false`,
        `cgp ${cgp}`, 'generate']);
      return messages.filter(m => m.type === 'output' && m.command === 'generate').at(-1).text;
    };
    const {SAMPLES} = await import('./analysis-model.mjs');
    const timings = {};
    const output = {};
    create();
    for (const [lexicon, source] of [['CSW24','build'], ['NWL23','download']]) {
      await engine.prepare(lexicon);
      const cgp = SAMPLES.peg.replace(/ -lex .*/, '');
      const baseline = await generate(lexicon, false, cgp);
      const before = performance.now();
      await engine.prepareWMP(lexicon, source, 4, true);
      timings[lexicon] = performance.now() - before;
      const withWMP = await generate(lexicon, true, cgp);
      output[lexicon] = {baseline, withWMP};
    }
    const files = await listWMPs();
    const manifest = await (await fetch('../data/wmp-manifest.json')).json();
    const hashesMatch = files.every(file => file.sha256 === manifest.lexica[file.lexicon].sha256);
    engine.worker.terminate();
    create();
    for (const lexicon of ['CSW24','NWL23']) {
      await engine.prepare(lexicon);
      await engine.prepareWMP(lexicon, 'build', 4, true);
      output[lexicon].cached = await generate(lexicon, true, SAMPLES.peg.replace(/ -lex .*/, ''));
      await engine.prepareWMP(lexicon, 'off', 4);
      output[lexicon].disabled = await generate(lexicon, false, SAMPLES.peg.replace(/ -lex .*/, ''));
    }
    const cachedLoads = messages.filter(m => m.type === 'wmp_ready' && m.fromCache && m.witFromCache).length;
    await engine.wmpCache('CSW24');
    const remaining = await listWMPs();
    await engine.wmpCache('NWL23');
    const empty = await listWMPs();
    engine.worker.terminate();
    return {timings, output, files, remaining, empty, cachedLoads, hashesMatch,
      warnings: messages.filter(m => m.type === 'wmp_cache_warning'),
      stages: messages.filter(m => m.type === 'wmp_progress').map(m => m.text)};
  });
  console.log(JSON.stringify({timings:report.timings, sizes:report.files.map(f=>[f.lexicon,f.bytes]), warnings:report.warnings, stages:report.stages.filter(s=>s.includes('Building'))}));
  expect(report.warnings).toEqual([]);
  expect(report.files.map(f=>f.lexicon).sort()).toEqual(['CSW24','NWL23']);
  expect(report.cachedLoads).toBe(2);
  expect(report.files.every(file => file.wit_bytes > 0 && file.wit_sha256.length === 64)).toBe(true);
  expect(report.hashesMatch).toBe(true);
  expect(report.stages).toContain('Building WIT indexes…');
  expect(report.remaining.map(f=>f.lexicon)).toEqual(['NWL23']);
  expect(report.empty).toEqual([]);
  for (const result of Object.values(report.output)) {
    expect(result.withWMP).toBe(result.baseline);
    expect(result.cached).toBe(result.baseline);
    expect(result.disabled).toBe(result.baseline);
  }
});

test('WMP settings cache and remove both lexica with file sizes', async ({page}) => {
  test.setTimeout(300000);
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await page.locator('#open-analysis-settings').click();
  await expect(page.getByRole('button', {name:'Cache CSW24 WMP', exact:true})).toBeDisabled();
  await expect(page.locator('#wmp-cache-enabled')).not.toBeChecked();
  await page.locator('#wmp-source').selectOption('build');
  await page.getByRole('button', {name:'Cache CSW24 WMP', exact:true}).click();
  await expect(page.getByRole('button', {name:'Remove CSW24 WMP', exact:true})).toBeEnabled({timeout:120000});
  await expect(page.locator('#wmp-source option[value="download"]')).toHaveCount(0);
  await page.getByRole('button', {name:'Cache NWL23 WMP', exact:true}).click();
  await expect(page.getByRole('button', {name:'Remove NWL23 WMP', exact:true})).toBeEnabled({timeout:120000});
  await expect(page.locator('#wmp-files')).toContainText('MiB');
  await expect(page.locator('#wmp-files')).toContainText('WIT');
  await page.screenshot({path:test.info().outputPath('wmp-settings.png'),fullPage:true});
  await page.locator('#back-to-game').click();
  await page.locator('[data-mode="kibitz"]').click();
  await expect(page.locator('#status')).toHaveText('Complete', {timeout:30000});
  await expect(page.locator('#error')).toBeHidden();
  await page.reload();
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await page.locator('#open-analysis-settings').click();
  await expect(page.getByRole('button', {name:'Remove CSW24 WMP', exact:true})).toBeEnabled();
  await page.getByRole('button', {name:'Remove CSW24 WMP', exact:true}).click();
  await expect(page.getByRole('button', {name:'Cache CSW24 WMP', exact:true})).toBeEnabled();
  await expect(page.getByRole('button', {name:'Remove NWL23 WMP', exact:true})).toBeEnabled();
});

test('bad downloads do not poison the engine or cache; native preparation can be stopped', async ({page}) => {
  test.setTimeout(60000);
  await page.route('**/wmp/*CSW24*.bin', route => route.fulfill({body:'truncated'}));
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  const report = await page.evaluate(async () => {
    const { EngineClient } = await import('./engine-client.mjs');
    const engine = new EngineClient();
    await engine.prepare('CSW24');
    let failure;
    try { await engine.prepareWMP('CSW24','download',2,true); }
    catch (error) { failure = error.message; }
    let stopped = false, progressSeen = false;
    engine.addEventListener('message', ({detail}) => {
      if (detail.type === 'wmp_progress' && detail.text.includes('Building')) {
        progressSeen = true;
        engine.stop();
      }
      if (detail.type === 'wmp_ready') stopped = detail.stopped;
    });
    await engine.prepareWMP('CSW24','build',1,true);
    await engine.run(['set -lex CSW24 -wit false -wmp false']);
    const files = (await engine.wmpCache()).files;
    engine.worker.terminate();
    return {failure, stopped, progressSeen, files};
  });
  expect(report.failure).toContain('checksum');
  expect(report.stopped).toBe(true);
  expect(report.progressSeen).toBe(true);
  expect(report.files).toEqual([]);
});

test('WMP format rejects truncated and mismatched assets before entering native code', async () => {
  const {validateWMP, wmpAsset} = await import('../wmp-assets.mjs');
  expect(() => validateWMP(new Uint8Array([3,15]))).toThrow('incomplete');
  expect(() => wmpAsset({schema:1,wmp_version:3,board_dim:15,lexica:{}}, 'CSW24','a'.repeat(64), 'https://example.com/data/wmp-manifest.json')).toThrow('does not match');
});

test('local builds need no hosted tables and caching is opt-in', async ({page}) => {
  test.setTimeout(90000);
  const unexpected = [];
  await page.route('**/data/wmp*', route => {
    unexpected.push(route.request().url());
    return route.abort();
  });
  await page.addInitScript(() => localStorage.setItem('magpie-wmp-source', 'download'));
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  await page.locator('#open-analysis-settings').click();
  await expect(page.locator('#wmp-source')).toHaveValue('build');
  await expect(page.locator('#wmp-cache-enabled')).not.toBeChecked();
  const report = await page.evaluate(async () => {
    const { EngineClient } = await import('./engine-client.mjs');
    const { listWMPs, readWMP } = await import('./wmp-cache.mjs');
    const engine = new EngineClient(), messages = [];
    engine.addEventListener('message', ({detail}) => messages.push(detail));
    await engine.prepare('CSW24');
    await engine.prepareWMP('CSW24', 'build', 4);
    const unsaved = await listWMPs();
    // Opting in also saves a map that was already prepared without caching.
    await engine.prepareWMP('CSW24', 'build', 4, true);
    const saved = await listWMPs();
    await engine.wmpCache('CSW24');
    const removed = await readWMP('CSW24', saved[0].kwg_sha256);
    // Removal must clear both stored blobs, not just their metadata.
    const keys = await new Promise((resolve, reject) => {
      const request = indexedDB.open('magpie-wmp', 1);
      request.onsuccess = () => {
        const db = request.result, tx = db.transaction('files');
        const read = tx.objectStore('files').getAllKeys();
        tx.oncomplete = () => { db.close(); resolve(read.result); };
        tx.onerror = () => reject(tx.error);
      };
    });
    engine.worker.terminate();
    return {unsaved, saved, removed, keys, stages: messages.filter(m => m.type === 'wmp_progress').map(m => m.text)};
  });
  expect(unexpected).toEqual([]);
  expect(report.unsaved).toEqual([]);
  expect(report.saved[0].wit_bytes).toBeGreaterThan(0);
  expect(report.removed).toBeNull();
  expect(report.keys).toEqual([]);
  expect(report.stages.some(stage => stage.startsWith('Building WMP'))).toBe(true);
  expect(report.stages).toContain('Building WIT indexes…');
});

test('WMP-only and damaged WIT caches are repaired locally', async ({page}) => {
  test.setTimeout(90000);
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  const report = await page.evaluate(async () => {
    const { EngineClient } = await import('./engine-client.mjs');
    const { listWMPs, readWMP, saveWMP } = await import('./wmp-cache.mjs');
    const { sha256 } = await import('./wmp-assets.mjs');
    const engine = new EngineClient(), messages = [];
    engine.addEventListener('message', ({detail}) => messages.push(detail));
    await engine.prepare('NWL23');
    await engine.prepareWMP('NWL23', 'build', 4, true);
    const modify = (legacy) => new Promise((resolve, reject) => {
      const request = indexedDB.open('magpie-wmp', 1);
      request.onsuccess = () => {
        const db = request.result, tx = db.transaction(['files', 'metadata'], 'readwrite');
        if (legacy) {
          tx.objectStore('files').delete('NWL23:wit');
          const read = tx.objectStore('metadata').get('NWL23');
          read.onsuccess = () => {
            delete read.result.wit_bytes; delete read.result.wit_sha256;
            tx.objectStore('metadata').put(read.result);
          };
        } else tx.objectStore('files').put(new Blob([new Uint8Array([4,15,0,0])]), 'NWL23:wit');
        tx.oncomplete = () => { db.close(); resolve(); };
        tx.onabort = () => { db.close(); reject(tx.error); };
      };
    });
    const repairs = [];
    for (const legacy of [true, false]) {
      await modify(legacy);
      engine.wmpKey = null;
      await engine.prepareWMP('NWL23', 'build', 4, true);
      const ready = messages.filter(message => message.type === 'wmp_ready').at(-1);
      const meta = (await listWMPs())[0];
      const saved = await readWMP('NWL23', meta.kwg_sha256);
      repairs.push({ready, valid: saved.wit.length > 100 && await sha256(saved.wit) === meta.wit_sha256});
    }
    const meta = (await listWMPs())[0];
    const saved = await readWMP('NWL23', meta.kwg_sha256);
    saved.wit[4] ^= 1; // Valid file/checksum, but wrong native KWG hash.
    await saveWMP('NWL23', meta.kwg_sha256, meta.sha256, 'build', saved.bytes, saved.wit, await sha256(saved.wit));
    engine.wmpKey = null;
    await engine.prepareWMP('NWL23', 'build', 4, true);
    const rebuilt = messages.filter(message => message.type === 'wmp_ready').at(-1);
    engine.worker.terminate();
    return {repairs, rebuilt};
  });
  expect(report.rebuilt).toMatchObject({fromCache:false, witFromCache:false, cached:true});
  for (const repair of report.repairs) {
    expect(repair.ready).toMatchObject({fromCache:true, witFromCache:false, cached:true});
    expect(repair.valid).toBe(true);
  }
});

test('cache provenance and upgrades are independent of the database schema', async ({page}) => {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready');
  const report = await page.evaluate(async () => {
    const cache = await import('./wmp-cache.mjs');
    const bytes = new Uint8Array([3,15,1,2,3,4]);
    await cache.saveWMP('NWL23','kwg','checksum','build',bytes,new Uint8Array([4,15]),'wit');
    const valid = !!await cache.readWMP('NWL23','kwg');
    const mismatch = await cache.readWMP('NWL23','other');
    // Exercise an actual schema upgrade over existing object stores.
    const source = (await (await fetch('./wmp-cache.mjs')).text()).replace('const DATABASE_VERSION = 1;', 'const DATABASE_VERSION = 2;');
    const upgraded = await import(URL.createObjectURL(new Blob([source], {type:'text/javascript'})));
    const survived = !!await upgraded.readWMP('NWL23','kwg');
    const staleSource = source.replace('wmp-wit-2026-10-review2','different-builder');
    const newerBuilder = await import(URL.createObjectURL(new Blob([staleSource], {type:'text/javascript'})));
    const stale = await newerBuilder.readWMP('NWL23','kwg');
    const stillStored = (await upgraded.listWMPs()).length;
    return {valid,mismatch,survived,stale,stillStored};
  });
  expect(report).toEqual({valid:true,mismatch:null,survived:true,stale:null,stillStored:1});
});
