import {test, expect} from '@playwright/test';

test('review edits preserve importable records and leave the engine usable after errors', async ({page}) => {
  await page.goto('/wasmentry/');
  await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  const report = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const {parseCGP,toCGP} = await import('./analysis-model.mjs');
    const engine = new EngineClient();
    await engine.prepare('CSW24');
    const empty = Array(15).fill('15').join('/');
    const header = '#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n';
    async function commit(text, index, move, rackOverride, cgpOverride) {
      const {game} = await engine.importGCG(text, 'CSW24');
      const snap = game.positions[index];
      const pos = parseCGP(snap.cgp);
      const rack = rackOverride ?? pos.racks[0];
      if (snap.onTurn) {pos.racks.reverse(); pos.scores.reverse();}
      if (rackOverride !== undefined) pos.racks[snap.onTurn] = rackOverride;
      try {
        const reply = await engine.gameAction({text,lexicon:'CSW24',action:1,index,move,rack,cgp:cgpOverride ?? toCGP(pos),onTurn:snap.onTurn,seed:42});
        const loaded = await engine.importGCG(reply.game.gcg,'CSW24');
        return {game:reply.game,reloaded:loaded.game};
      } catch(error) {return {error:error.message};}
    }
    try {
      const standard = await (await fetch('./tests/fixtures/standard.gcg')).text();
      const exchanges = [];
      for (const move of ['ex I','ex ILOOPQ','ex ILOOPQR']) {
        exchanges.push(await commit(standard,22,move));
      }
      const outplay = await commit(standard,26,'D11 .OTS');
      const finalPass = outplay.game && await commit(outplay.game.gcg,outplay.game.index,'pass');
      const challenge = outplay.game && await engine.gameAction({text:outplay.game.gcg,lexicon:'CSW24',action:3,index:outplay.game.index});
      const branch = outplay.game && await commit(outplay.game.gcg,22,'pass');
      const later = branch?.game && await commit(branch.game.gcg,branch.game.index,'pass','EEIIPRU');
      const stale = header + '>A: AEINRST 8H TRAIN +12 12\n>B: AELNOST - +0 0\n#rack1 JKVWXYZ\n';
      const replaced = await commit(stale,1,'pass');
      const corrected = replaced.game && await commit(replaced.game.gcg,2,'pass','DENOQUU');
      const initial = header + `#cgp ${empty} AEINRST/BCDFGHJ 0/0 0\n`;
      const firstPass = await commit(initial,0,'pass');
      const conflict = await commit(firstPass.game.gcg,1,'pass','EILNOTU');
      const valid = await commit(firstPass.game.gcg,1,'pass');
      const opening = await commit(header + `#cgp ${empty} AEINRST/ 0/0 0\n`,0,'8H TRAIN');
      const unknownPass = await commit(opening.game.gcg,1,'pass');
      const suppliedPass = await commit(opening.game.gcg,1,'pass','BCDFGHJ');
      return {exchanges,outplay,finalPass,challenge,branch,later,replaced,corrected,conflict,valid,unknownPass,suppliedPass};
    } finally {engine.worker.terminate();}
  });
  await test.info().attach('review-records', {body:JSON.stringify(report,null,2),contentType:'application/json'});
  for (const result of report.exchanges) expect(result.error).toContain('cannot exchange with fewer than 7');
  expect(report.outplay.error).toBeUndefined();
  expect(report.outplay.game.ended).toBe(false);
  expect(report.outplay.reloaded.ended).toBe(false);
  expect(report.finalPass.error).toBeUndefined();
  expect(report.finalPass.game.ended).toBe(true);
  expect(report.finalPass.game.gcg).toContain('+2 378');
  expect(report.challenge.game).toBeDefined();
  expect(report.branch.error).toBeUndefined();
  expect(report.branch.game.gcg).not.toContain('#rack1 I\n');
  expect(report.branch.game.positions.at(-1).cgp.split(' ')[1].split('/')[0]).toBe('');
  expect(report.later.error).toBeUndefined();
  expect(report.replaced.error).toBeUndefined();
  expect(report.replaced.game.gcg).not.toContain("JKVWXYZ");
  expect(report.corrected.error).toBeUndefined();
  expect(report.conflict.error).toMatch(/conflict/);
  expect(report.valid.error).toBeUndefined();
  expect(report.unknownPass.error).toContain('rack before recording a pass');
  expect(report.suppliedPass.error).toBeUndefined();
  for (const result of [report.outplay,report.finalPass,report.branch,report.later,report.valid,report.suppliedPass]) {
    expect(result.reloaded.positions.at(-1).cgp).toBe(result.game.positions.at(-1).cgp);
  }
});
