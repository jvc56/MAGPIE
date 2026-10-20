import { test, expect } from '@playwright/test';
import { pegThreadsForMemory } from '../peg-memory.mjs';

test('PEG uses current memory headroom and preserves the requested count when it fits', {tag:'@unit'}, () => {
  const probes = [];
  const select = maximum => pegThreadsForMemory(4, count => { probes.push(count); return count <= maximum; });
  expect(select(4)).toBe(4);
  expect(probes.splice(0)).toEqual([4]);
  expect(select(2)).toBe(2);
  expect(probes.splice(0)).toEqual([4,2]);
  expect(select(1)).toBe(1);
  expect(probes.splice(0)).toEqual([4,2,1]);
  expect(() => select(0)).toThrow('Not enough WASM memory');
  expect(pegThreadsForMemory(1, () => true)).toBe(1);
  expect(() => pegThreadsForMemory(33, () => true)).toThrow('between 1 and 32');
});

for (const scenario of [
  {name:'desktop with a 256 MiB ceiling', ceiling:256, mobile:false, expected:4, resumedAtLeast:2},
  {name:'mobile with a 512 MiB ceiling', ceiling:512, mobile:true, expected:4},
  {name:'desktop after building maps and simulating', ceiling:1024, mobile:false, warm:true, expected:4},
  {name:'desktop retaining maps after simulation', ceiling:1024, mobile:false, warm:true, maps:true, expected:4},
  {name:'mobile with a 1 GiB ceiling', ceiling:1024, mobile:true, expected:4},
  {name:'desktop with 256 MiB already allocated in a 512 MiB heap', ceiling:512, mobile:false, heldMiB:256, expected:4, resumedAtLeast:2},
]) {
  test(`PEG chooses workers by allocatable memory on ${scenario.name}`, async ({page}) => {
    test.setTimeout(90000);
    await page.route('**/wasm-worker.js', async route => {
      const response = await route.fetch();
      const setup = `
        Object.defineProperty(navigator, 'userAgent', {get: () => ${JSON.stringify(scenario.mobile ? 'iPhone' : 'Desktop')}});
        const originalPost = self.postMessage.bind(self);
        self.postMessage = function(message, ...args) {
          if (message.type === 'init_complete' && ${scenario.heldMiB || 0}) {
            self.testHeldMemory = Module._malloc(${scenario.heldMiB || 0} * 1024 * 1024);
            if (!self.testHeldMemory) throw new Error('Could not create test memory pressure');
          }
          return originalPost(message, ...args);
        };
        WebAssembly.Memory = new Proxy(WebAssembly.Memory, {
          construct(target,args) {
            if (args[0].maximum > ${scenario.ceiling * 16}) throw new RangeError('Test heap ceiling');
            return Reflect.construct(target,args);
          }
        });\n`;
      await route.fulfill({response,body:setup + await response.text()});
    });
    await page.goto('/');
    const report = await page.evaluate(async ({warm,maps}) => {
      const {EngineClient} = await import('/wasmentry/engine-client.mjs');
      const {analysisCommands,parseCGP,SAMPLES} = await import('/wasmentry/analysis-model.mjs');
      const engine = new EngineClient();
      const budgets = [];
      const started = [];
      engine.addEventListener('message', ({detail}) => {
        if (detail.type === 'peg_memory_budget') budgets.push(detail);
        if (detail.type === 'command_started') started.push(detail.command);
      });
      try {
        await engine.prepare('CSW24');
        await engine.prepareWMP('CSW24',warm ? 'build' : 'off',4);
        const snapshots = [{phase:'prepared',...engine.memory}];
        if (warm) {
          if (engine.wmpUnavailable) throw new Error("Maps were not installed");
          await engine.run(analysisCommands(parseCGP(SAMPLES.opening),'sim', {threads:4,ttMiB:32,seconds:1,plies:2,candidates:10,wmp:true}));
          snapshots.push({phase:'simulated',...engine.memory});
        }
        const board = '3J1OBE7/1DEAIRING6/1I1V11/MA1E11/IT1L11/nO8L4/UM3WAUK1O4/ES1R3PENNY3/T2E1YOOF1G4/EARLIES2ASH3/D2I3Z1AHA3/3C3O2iN3/3T3O2P4/AXE1REGIFTS4/WINCER1D7';
        const position = parseCGP(`${board} EIOQRUU/ADLNTTV 406/421 0 -lex CSW24`);
        const settings = {threads:4,ttMiB:16,seconds:1,plies:2,candidates:10,wmp:!!maps,capturePegOutcomes:false};
        await engine.run(analysisCommands(position,'peg',settings));
        // Both fresh and resumed searches pass through the same launch guard.
        await engine.run(analysisCommands(position,'peg',{...settings,resumePeg:true}));
        await engine.run(['generate']);
        return {snapshots,budgets,started,memory:engine.memory,fatal:engine.fatalError?.message};
      } finally {engine.worker.terminate();}
    }, {warm:scenario.warm,maps:scenario.maps});
    await test.info().attach("memory-budget", {body:JSON.stringify(report,null,2), contentType:"application/json"});
    expect(report.fatal).toBeUndefined();
    expect(report.budgets).toHaveLength(2);
    expect(report.budgets[0]).toMatchObject({requested:4,threads:scenario.expected});
    // The resumed search passes the same guard. In the two tightest heaps four
    // workers fit with about 25 MiB to spare, and the first solve can leave the
    // free memory in pieces smaller than the guard's 8 MiB probes, so the resumed
    // search may get fewer workers than the fresh one, though never fewer than
    // the two these heaps got before the scratch reserve was halved.
    expect(report.budgets[1].requested).toBe(4);
    expect(report.budgets[1].threads).toBeLessThanOrEqual(scenario.expected);
    expect(report.budgets[1].threads).toBeGreaterThanOrEqual(scenario.resumedAtLeast ?? scenario.expected);
    expect(report.started.some(command => command.startsWith('peg resume '))).toBe(true);
    expect(report.memory.heapBytes).toBeLessThanOrEqual(scenario.ceiling * 1024 * 1024);
  });
}
