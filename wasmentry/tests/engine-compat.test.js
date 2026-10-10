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
  await page.addInitScript(() => localStorage.setItem('magpie-wmp-source', 'build'));
  await page.route('**/wasm-worker.js',async route=>{
    const response=await route.fetch();
    await route.fulfill({response,body:`WebAssembly.Memory = new Proxy(WebAssembly.Memory, {construct(target,args){if(args[0].maximum>4096)throw new RangeError('Test reservation limit');return Reflect.construct(target,args);}});\n${await response.text()}`});
  });
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Complete',{timeout:30000});
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
