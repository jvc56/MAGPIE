import { test, expect } from "@playwright/test";
import { readFile } from "node:fs/promises";
import vm from "node:vm";
import { combineSim, analysisPositionKey, parseCGP, SAMPLES } from "../analysis-model.mjs";

test("pooling preserves missing and zero-iteration rows; rack order does not split caches", () => {
  const a = {move:"8H TRAIN",iterations:500,win:50,equity:20,winSE:1,equitySE:2};
  const b = {...a,move:"8H RAIN",iterations:700};
  expect(combineSim([a,b],[{...a,iterations:0}]).map(({rank,...row})=>row)).toEqual([a,b]);
  const combined=combineSim([a,b],[{...a,win:60}]);
  expect(combined[0]).toMatchObject({iterations:1000,win:55});
  expect(combined[1]).toMatchObject(b);
  const position=parseCGP(SAMPLES.opening);
  const key=analysisPositionKey(position);
  position.racks[0]=[...position.racks[0]].reverse().join("");
  expect(analysisPositionKey(position)).toBe(key);
});

async function clientHarness() {
  const source=await readFile("engine-client.mjs","utf8");
  class Worker {
    postMessage(message) { this.sent=message; }
    terminate() { this.terminated=true; }
    emit(data) { this.onmessage({data}); }
  }
  const timers=[];
  const context={Worker,URL,EventTarget,CustomEvent, setTimeout:(fn)=>{timers.push(fn);return fn},clearTimeout:()=>{}};
  const EngineClient=vm.runInNewContext(source.replace("export class", "class").replaceAll("import.meta.url",'"http://localhost/wasmentry/engine-client.mjs"')+'\nEngineClient;',context);
  return {client:new EngineClient(),timers};
}

test("request IDs reject stale output, destroy resets caches, and fatal abort rejects future requests", async () => {
  const {client}=await clientHarness();
  client.worker.emit({type:"ready"});
  await client.ready;
  const messages=[];client.addEventListener("message",({detail})=>messages.push(detail));
  const pending=client.run(["generate"]);
  const id=client.worker.sent.requestId;
  client.worker.emit({type:"output",requestId:id-1,text:"stale"});
  expect(messages).toHaveLength(0);
  client.worker.emit({type:"complete",requestId:id});await pending;
  client.loadedFiles.add("foo");client.initialized=true;client.wmpKey="CSW24:build";
  const destroy=client.destroy();client.worker.emit({type:"destroyed",requestId:client.worker.sent.requestId});await destroy;
  expect(client.loadedFiles.size).toBe(0);expect(client.initialized).toBe(false);expect(client.wmpKey).toBeNull();
  const run=client.run(["sim"]);
  client.worker.emit({type:"fatal",text:"Aborted(OOM)"});
  expect(await run.catch(e=>e.message)).toMatch(/Reload/);
  expect(client.worker.terminated).toBe(true);
  expect(await client.run(["generate"]).catch(e=>e.message)).toMatch(/Reload/);
});

test("an unresponsive native import times out and terminates the worker", async () => {
  const {client,timers}=await clientHarness();
  const load=client.importGCG("text","CSW24");
  timers.at(-1)();
  expect(await load.catch(e=>e.message)).toMatch(/timed out/);
  expect(client.worker.terminated).toBe(true);
});

async function ready(page) {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready");
}

test("real engine can warm sixteen search threads and reinitialize after destroy", async ({page}) => {
  test.setTimeout(60000);await ready(page);
  const result=await page.evaluate(async()=>{
    const {EngineClient}=await import("/wasmentry/engine-client.mjs");
    const {analysisCommands,parseCGP,SAMPLES}=await import("/wasmentry/analysis-model.mjs");
    const engine=new EngineClient();const output=[];engine.addEventListener("message",({detail})=>{if(detail.type==='output')output.push(detail.text)});
    try {
      await engine.prepare("CSW24");
      await engine.run(analysisCommands(parseCGP(SAMPLES.opening),"sim",{threads:16,seconds:.1,plies:2,candidates:2}));
      await engine.destroy();await engine.prepare("CSW24");
      await engine.run(analysisCommands(parseCGP(SAMPLES.opening),"kibitz",{threads:1,seconds:.1,plies:2,candidates:2}));
      return output.join("\n");
    } finally {engine.worker.terminate()}
  });
  expect(result).toContain("Iters:");expect(result).toMatch(/TRAIN|RETAIN|NASTIER/);
});

test("Kibitz then Continue then immediate Stop preserves the accumulated sim", async ({page})=>{
  await ready(page);
  await page.locator("#gcg-file").setInputFiles("tests/fixtures/standard.gcg");
  await expect(page.locator("#game-history")).toBeVisible();
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("1");await page.locator("#candidates").fill("3");
  await page.locator("#back-to-game").click();
  await page.locator('[data-mode=sim]').click();await expect(page.locator('#status')).toHaveText('Complete');
  const total=()=>page.locator('#analysis-meta').textContent().then(s=>Number(s.match(/([\d,]+) total iterations/)[1].replaceAll(',','')));
  const before=await total();
  await page.locator('[data-mode=kibitz]').click();await expect(page.locator('#status')).toHaveText('Complete');
  await page.evaluate(()=>{document.querySelector('[data-mode=sim]').click();document.getElementById('stop').click()});
  await expect(page.locator('#stop')).toBeDisabled();expect(await total()).toBeGreaterThanOrEqual(before);
  await page.locator('#history-next').click();await page.locator('#history-previous').click();
  expect(await total()).toBeGreaterThanOrEqual(before);
});

test("live play conceals exports and prohibits clock-refunding undo", async ({page})=>{
  await ready(page);await page.locator('#new-game').click();await page.locator('[data-game-kind=play]').click();
  await page.locator('#new-game-form button.primary').click();await expect(page.locator('#play-session')).toBeVisible();
  await expect(page.locator('#undo-game')).toBeDisabled();await expect(page.locator('#save-gcg')).toBeDisabled();await expect(page.locator('#copy-gcg')).toBeDisabled();
  await page.locator('#finish-game').click();await page.locator('#confirm-yes').click();
  await expect(page.locator('#session-result')).toContainText(/resign/i);
  await expect(page.locator('#undo-game')).toBeDisabled();await expect(page.locator('#save-gcg')).toBeEnabled();
});

test("late move submission cannot undo a loss on time", async ({page})=>{
  await page.addInitScript(()=>{
    const Original=Worker;
    window.Worker=class extends Original {
      set onmessage(handler) {super.onmessage=event=>{
        if(window.holdGameReply && event.data.type==='game_updated') window.releaseGameReply=()=>handler(event);
        else handler(event);
      }}
    };
  });
  await page.clock.install();await ready(page);
  await page.locator('#new-game').click();await page.locator('[data-game-kind=play]').click();
  await page.locator('.game-options > summary').click();await page.selectOption('#game-time','300');await page.selectOption('#overtime','0');await page.selectOption('#first-player','0');
  await page.locator('#new-game-form button.primary').click();await expect(page.locator('#play-session')).toBeVisible();
  await page.evaluate(()=>{window.holdGameReply=true});await page.locator('#pass-move').click();
  await page.waitForFunction(()=>Boolean(window.releaseGameReply));
  await page.clock.fastForward(301000);
  await expect(page.locator('#session-result')).toContainText('lost on time');
  const count=await page.locator('#history-position option').count();
  await page.evaluate(()=>{window.holdGameReply=false;window.releaseGameReply()});
  await expect(page.locator('#session-result')).toContainText('lost on time');
  await expect(page.locator('#history-position option')).toHaveCount(count);
  await expect(page.locator('#move-entry')).toBeHidden();
});

test("branching rejects an edited recorded board", async ({page})=>{
  await ready(page);await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#game-history')).toBeVisible();
  await page.locator('#position-editor > summary').click();
  await page.locator('#score').fill('999');await page.locator('#score').dispatchEvent('change');
  const count=await page.locator('#history-position option').count();
  await page.locator('#play-from').click();
  await expect(page.locator('#error')).not.toBeEmpty();
  await expect(page.locator('#play-session')).toBeHidden();
  await expect(page.locator('#history-position option')).toHaveCount(count);
});

test("automatic review keeps pooled sims and spoiler hiding omits played-move injection", async ({page})=>{
  await page.addInitScript(()=>{window.commands=[];const post=Worker.prototype.postMessage;Worker.prototype.postMessage=function(message,...args){if(message.type==='run')window.commands.push(...message.data.commands);return post.call(this,message,...args)}});
  await ready(page);await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');await expect(page.locator('#game-history')).toBeVisible();
  await page.locator('#open-analysis-settings').click();await page.locator('#seconds').fill('1');await page.locator('#candidates').fill('3');await page.selectOption('#auto-analyze','auto');await page.locator('#back-to-game').click();
  await page.locator('[data-mode=sim]').click();await expect(page.locator('#status')).toHaveText('Complete');
  const total=()=>page.locator('#analysis-meta').textContent().then(s=>Number(s.match(/([\d,]+) total iterations/)[1].replaceAll(',','')));
  const before=await total();await page.locator('#history-next').click();await expect(page.locator('#stop')).toBeDisabled();
  await page.locator('#history-previous').click();await expect(page.locator('#stop')).toBeDisabled();expect(await total()).toBeGreaterThan(before);
  await page.locator('#hide-spoilers').check();await page.evaluate(()=>{window.commands=[]});
  await page.locator('[data-mode=sim]').click();await expect(page.locator('#stop')).toBeDisabled();
  const commands=await page.evaluate(()=>window.commands);
  expect(commands).toContain('sim');expect(commands.some(c=>c.startsWith('addmoves ')||c.startsWith('snoprune '))).toBe(false);
});

test("watch One turn restores the live position and Resume works with auto-analysis", async ({page})=>{
  await ready(page);await page.locator('#new-game').click();await page.locator('[data-game-kind=watch]').click();await page.locator('.game-options > summary').click();await page.selectOption('#bot-analysis','kibitz');await page.selectOption('#game-time','0');await page.locator('#watch-delay').fill('1');await page.locator('#new-game-form button.primary').click();await expect(page.locator('#play-session')).toBeVisible();
  await page.locator('#pause-game').click();await expect(page.locator('#stop')).toBeDisabled();
  await page.locator('#step-game').click();await expect(page.locator('#stop')).toBeDisabled();await expect(page.locator('#history-position option')).toHaveCount(2);
  await page.locator('#open-analysis-settings').click();await page.selectOption('#auto-analyze','kibitz');await page.locator('#back-to-game').click();
  await page.locator('#history-first').click();await expect(page.locator('#stop')).toBeDisabled();
  await page.locator('#step-game').click();await expect(page.locator('#stop')).toBeDisabled();await expect(page.locator('#history-position option')).toHaveCount(3);
  await page.locator('#history-first').click();await expect(page.locator('#stop')).toBeDisabled();await page.locator('#pause-game').click();
  await expect.poll(()=>page.locator('#history-position option').count()).toBeGreaterThan(3);
  await page.locator('#pause-game').click();
});

test('engine failure leaves the recorded game downloadable', async ({page}) => {
  await page.addInitScript(() => {
    const Original = Worker;
    window.Worker = class extends Original {
      constructor(url, options) { super(url, options); this.isEngine = String(url).includes('wasm-worker.js'); }
      set onmessage(handler) {
        super.onmessage = handler;
        if (this.isEngine) window.failEngine = () => handler({data:{type:'fatal',text:'Test engine failure'}});
      }
    };
  });
  await ready(page);
  await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#game-history')).toBeVisible();
  const players = await page.locator('#game-players').textContent();
  await page.evaluate(() => window.failEngine());
  await expect(page.locator('#status')).toContainText('Engine unavailable');
  await expect(page.locator('#save-gcg')).toBeEnabled();
  await expect(page.locator('#copy-gcg')).toBeEnabled();
  await page.locator('#game-menu > summary').click();
  const download = page.waitForEvent('download');
  await page.locator('#save-gcg').click();
  const stream = await (await download).createReadStream();
  const chunks = []; for await (const chunk of stream) chunks.push(chunk);
  const gcg = Buffer.concat(chunks).toString();
  expect(gcg).toContain('#player1'); expect(gcg).toContain('>');
  await expect(page.locator('#game-players')).toHaveText(players);
});

test('replacing a live game requires confirmation and cancel preserves it', async ({page}) => {
  await ready(page);
  await page.locator('#new-game').click();
  await page.locator('[data-game-kind=play]').click();
  await page.locator('.game-options > summary').click();
  await page.selectOption('#first-player','0');
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#play-session')).toBeVisible();
  const rack = await page.locator('#rack-tiles').textContent();
  await page.locator('#new-game').click();
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#game-confirm')).toBeVisible();
  await page.locator('#confirm-no').click();
  await page.locator('#cancel-new-game').click();
  await expect(page.locator('#rack-tiles')).toHaveText(rack);
  await expect(page.locator('#play-session')).toBeVisible();
});

test('a native WASM abort rejects requests and ends the worker', async ({page}) => {
  await page.route('**/wasm-worker.js', async route => {
    const response = await route.fetch();
    await route.fulfill({response, body: await response.text() + '\nself.addEventListener("message", event => { if (event.data.type === "test-native-abort") api.precache(0, 0, -1); });'});
  });
  await ready(page);
  const result = await page.evaluate(async () => {
    const {EngineClient} = await import('/wasmentry/engine-client.mjs');
    const engine = new EngineClient();
    await engine.prepare('CSW24');
    const fatal = new Promise(resolve => engine.addEventListener('message', ({detail}) => {if (detail.type === 'fatal') resolve(detail.text)}));
    engine.worker.postMessage({type:'test-native-abort'});
    const text = await fatal;
    const rejected = await engine.run(['generate']).catch(error => error.message);
    return {text,rejected};
  });
  expect(result.text).toMatch(/Aborted|abort/i);
  expect(result.rejected).toMatch(/Reload/);
});

test('a low-memory WMP refusal preserves ordinary analysis', async () => {
  const {client} = await clientHarness();
  client.worker.emit({type:'ready'});
  const prepare = client.prepareWMP('CSW24','build',2);
  await Promise.resolve();
  client.worker.emit({type:'complete',requestId:client.worker.sent.requestId});
  await Promise.resolve(); await Promise.resolve();
  client.worker.emit({type:'wmp_ready',requestId:client.worker.sent.requestId,unavailable:true});
  await prepare;
  const run = client.run(['set -wmp true -wit true','generate']);
  expect(client.worker.sent.data.commands[0]).toBe('set -wmp false -wit false');
  client.worker.emit({type:'complete',requestId:client.worker.sent.requestId}); await run;
});


test("a pending history click is applied before starting analysis", async ({page}) => {
  await ready(page);
  await page.locator('#gcg-file').setInputFiles('tests/fixtures/standard.gcg');
  await expect(page.locator('#game-history')).toBeVisible();
  await page.locator('#history-last').click();
  await page.evaluate(() => {
    document.querySelector('[data-event-index="0"]').dispatchEvent(new MouseEvent('click', {bubbles:true, detail:1}));
    document.querySelector('[data-mode="kibitz"]').click();
  });
  await expect(page.locator('#status')).toHaveText('Complete');
  await expect(page.locator('#history-position')).toHaveValue('0');
});
