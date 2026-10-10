import { test, expect } from "@playwright/test";

test("shared-memory engine starts and generates moves at a mobile viewport", async ({page}) => {
  await page.setViewportSize({width:390,height:844});
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Complete',{timeout:30000});
  await expect(page.locator('#results tbody tr')).toHaveCount(100);
});

test("smaller shared-memory reservations retain baseline move generation", async ({page})=>{
  await page.setViewportSize({width:390,height:844});
  await page.addInitScript(() => {
    localStorage.setItem('magpie-wmp-source', 'build');
    Object.defineProperty(navigator, 'userAgent', {get: () => 'iPhone'});
    Object.defineProperty(navigator, 'deviceMemory', {get: () => undefined});
    window.engineCommands = [];
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      if (message.type === 'run') window.engineCommands.push(...message.data.commands);
      return post.call(this, message, ...args);
    };
  });
  await page.route('**/wasm-worker.js',async route=>{
    const response=await route.fetch();
    await route.fulfill({response,body:`WebAssembly.Memory = new Proxy(WebAssembly.Memory, {construct(target,args){if(args[0].maximum>4096)throw new RangeError('Test reservation limit');return Reflect.construct(target,args);}});\n${await response.text()}`});
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Complete',{timeout:30000});
  await expect(page.locator('#results tbody tr')).toHaveCount(100);
  await expect(page.locator('#error')).toBeHidden();
  await expect(page.locator('#wmp-cache-status')).toContainText('Using standard move generation');
  const commands = await page.evaluate(() => window.engineCommands);
  expect(commands.some(command => /-(wmp|wit) true/.test(command))).toBe(false);
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Complete');
  await expect(page.locator('#results tbody tr')).toHaveCount(100);
});


test("mobile defaults can cancel a word-map build and keep the engine usable", async ({page}) => {
  test.setTimeout(120000);
  await page.setViewportSize({width:390,height:844});
  await page.addInitScript(() => {
    Object.defineProperty(navigator, 'userAgent', {get: () => 'iPhone'});
    Object.defineProperty(navigator, 'deviceMemory', {get: () => undefined});
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready', {timeout:30000});
  await expect(page.locator('#tt-memory')).toHaveValue('16');
  const result = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const engine = new EngineClient();
    const messages = [];
    engine.addEventListener('message', ({detail}) => {
      messages.push(detail);
      if (detail.type === 'wmp_progress' && /word entries|single blanks|double blanks/.test(detail.text)) engine.stop();
    });
    try {
      await engine.prepare('NWL23');
      await engine.prepareWMP('NWL23', 'build', 2, false);
      await engine.run(['set -lex NWL23 -wmp false -wit false', 'cgp 15/15/15/15/15/15/15/15/15/15/15/15/15/15/15 AEINRST/ 0/0 0', 'generate']);
      return {stages:messages.filter(message => message.type === "wmp_progress").map(message => message.text), stopped:messages.some(message => message.type === 'wmp_ready' && message.stopped), output: messages.filter(message => message.type === 'output').map(message => message.text).join('')};
    } finally {engine.worker.terminate();}
  });
  expect(result.stopped, JSON.stringify(result.stages)).toBe(true);
  expect(result.output).toMatch(/TRAIN|RETAIN|NASTIER/);
});

// Fault injection targets the two actual native input allocations. The cache
// contains real engine-built tables; normal move generation and sim stay real.
for (const allocation of ['WMP', 'WIT']) {
  test(`mobile ${allocation} allocation failure preserves cache and falls back to baseline analysis`, async ({page}) => {
    test.setTimeout(120000);
    await page.setViewportSize({width:390,height:844});
    await page.addInitScript(() => {
      Object.defineProperty(navigator, 'userAgent', {get: () => 'iPhone'});
      Object.defineProperty(navigator, 'deviceMemory', {get: () => undefined});
    });
    await page.route('**/wasm-worker.js', async route => {
      const response = await route.fetch();
      const source = await response.text();
      const allocationCall = allocation === 'WMP' ? '      ptr = Module._malloc(bytes.length);' : '      witPtr = Module._malloc(wit.length);';
      expect(source).toContain(allocationCall);
      await route.fulfill({response, body:source.replace(allocationCall,
        allocation === 'WMP' ? 'ptr = 0;' : 'witPtr = 0;')});
    });
    await page.goto('/wasmentry/');
    await expect(page.locator('#status')).toHaveText('Engine ready');
    const result = await page.evaluate(async () => {
      const {EngineClient} = await import('./engine-client.mjs');
      const {listWMPs} = await import('./wmp-cache.mjs');
      const {SAMPLES, parseCGP, analysisCommands} = await import('./analysis-model.mjs');
      const engine = new EngineClient(), messages = [], sent = [];
      engine.addEventListener('message', ({detail}) => messages.push(detail));
      const post = engine.worker.postMessage.bind(engine.worker);
      engine.worker.postMessage = message => {sent.push(message); post(message);};
      try {
        await engine.prepare('NWL23');
        // The build path has no input-file allocation, so it can seed the cache.
        await engine.prepareWMP('NWL23', 'build', 2, true);
        const before = await listWMPs();
        await engine.prepareWMP('NWL23', 'off', 2);
        const position = parseCGP(SAMPLES.opening.replace('CSW24', 'NWL23'));
        const settings = {threads:2,seconds:0.1,plies:1,candidates:3,ttMiB:16,wmp:false};
        await engine.run(analysisCommands(position, 'kibitz', settings));
        const baseline = messages.filter(m => m.type === 'output' && m.command === 'generate').at(-1).text;
        await engine.prepareWMP('NWL23', 'build', 2, true);
        const attempts = sent.filter(m => m.type === 'prepare_wmp').length;
        // A failed load must not be retried on every analysis, even with caching on.
        await engine.prepareWMP('NWL23', 'build', 2, true);
        const retried = sent.filter(m => m.type === 'prepare_wmp').length !== attempts;
        const start = sent.length;
        await engine.run(analysisCommands(position, 'kibitz', {...settings,wmp:true}));
        const fallback = messages.filter(m => m.type === 'output' && m.command === 'generate').at(-1).text;
        await engine.run(analysisCommands(position, 'sim', {...settings,wmp:true}));
        const simulation = messages.filter(m => m.type === 'output' && m.command === 'sim').at(-1)?.text;
        return {before, after:await listWMPs(), baseline, fallback, retried,
          simulation, unavailable:engine.wmpUnavailable,
          ready:messages.filter(m => m.type === "wmp_ready"),
          fatal:engine.fatalError?.message,
          warnings:messages.filter(m => m.type === 'wmp_cache_warning').map(m => m.text),
          commands:sent.slice(start).filter(m => m.type === 'run').flatMap(m => m.data.commands)};
      } finally {engine.worker.terminate();}
    });
    expect(result.unavailable, JSON.stringify({ready:result.ready, warnings:result.warnings, files:result.before})).toBe(true);
    expect(result.fatal).toBeUndefined();
    expect(result.retried).toBe(false);
    expect(result.before).toHaveLength(1);
    expect(result.after).toEqual(result.before);
    expect(result.warnings).toEqual([expect.stringContaining('Not enough memory')]);
    expect(result.fallback).toBe(result.baseline);
    expect(result.simulation).toMatch(/Iters:\s+[1-9]\d*/);
    expect(result.commands.some(command => /-(wmp|wit) true/.test(command))).toBe(false);
    await page.locator('[data-mode=kibitz]').click();
    await expect(page.locator('#status')).toHaveText('Complete');
  });
}
