import {test, expect} from '@playwright/test';
test('challenge settings default to five per play; failed live challenge awards bonus', async ({page}) => {
  await page.goto('/wasmentry/'); await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
  // A random opening rack can make the bot exchange, which cannot be challenged.
  await page.locator('#new-game-position > summary').click();
  await page.locator('#new-game-cgp').fill(`${Array(15).fill('15').join('/')} AEINRST/BCDFGHJ 0/0 0 -lex CSW24`);
  await page.locator('.game-options > summary').click();
  await expect(page.locator('#challenge-rule option')).toHaveText(['VOID','PENALTY','SINGLE','DOUBLE']);
  await page.locator('#challenge-rule').selectOption('penalty');
  await expect(page.locator('#challenge-points')).toHaveValue('5'); await expect(page.locator('#challenge-unit')).toHaveValue('play');
  await page.locator('#game-time').selectOption('0'); await page.locator('#bot-analysis').selectOption('kibitz'); await page.locator('#human-side').selectOption('1');
  await page.locator('#start-game').click(); await expect(page.locator('#challenge-move')).toBeEnabled({timeout:30000});
  const before=Number(await page.locator('#history-table .history-cumulative').first().textContent());
  await page.locator('#challenge-move').click(); await expect(page.locator('#status')).toHaveText('Challenge recorded');
  await expect(page.locator('#history-table .history-cumulative').last()).toHaveText(String(before+5));
  await expect(page.locator('#challenge-move')).toBeHidden(); await expect(page.locator('#play-move')).toBeEnabled(); await expect(page.locator('#error')).toBeHidden();
});
test('WASM challenge rules preserve racks and reimport every failed-challenge outcome', async ({page}) => {
  await page.goto('/wasmentry/');
  const report=await page.evaluate(async()=>{
    const {EngineClient}=await import('/wasmentry/engine-client.mjs'); const {GameAction}=await import('/wasmentry/game-actions.mjs'); const engine=new EngineClient();
    try {
      await engine.prepare('CSW24'); const rows=Array(15).fill('15'); rows[7]='7AT6';
      const text=`#lexicon CSW24\n#player1 A Alice\n#player2 B Bob\n#cgp ${rows.join('/')} AEINRTT/BCDFGHJ 0/0 0\n>A: AEINRTT 9H TE +8 8\n`;
      rows[8]='7TE6'; const cgp=`${rows.join('/')} AINRTXY/BCDFGHJ 8/0 0`; const outputs=[];
      for(const [challenge,challengePoints,challengeUnit] of [['single',5,'play'],['double',5,'play'],['penalty',5,'play'],['penalty',10,'play'],['penalty',5,'word'],['penalty',10,'word']]) {
        const {game}=await engine.gameAction({text,lexicon:'CSW24',index:1,cgp,onTurn:1,action:GameAction.LIVE_CHALLENGE,challenge,challengePoints,challengeUnit});
        const {game:again}=await engine.importGCG(game.gcg,'CSW24'); outputs.push({gcg:game.gcg,last:game.positions.at(-1),reimport:again.positions.at(-1)});
      } return outputs;
    } finally {engine.worker.terminate();}
  });
  for(const [index,bonus] of [0,0,5,10,15,30].entries()) {
    const result=report[index]; expect(result.gcg).toContain(`(challenge) +${bonus} ${8+bonus}`);
    expect(result.last.onTurn).toBe(index===1?0:1);
    expect(result.last.cgp).toContain(index===1?' AINRTXY/BCDFGHJ 8/0 1':` BCDFGHJ/AINRTXY 0/${8+bonus} 0`);
    expect(result.reimport.onTurn).toBe(result.last.onTurn);
  }
});
