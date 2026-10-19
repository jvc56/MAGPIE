import { test, expect } from "@playwright/test";
import { readFileSync } from "node:fs";
import { emptyPosition, inventory, parseCGP, toCGP, analysisCommands } from "../analysis-model.mjs";
import { tileCounts, tileValues, vowelLetters } from "../lexica.mjs";
import { translate, preferredLanguage } from "../i18n.mjs";
import { french } from "../fr.mjs";
import { openGameActions } from "./helpers/startup.js";

const empty = Array(15).fill("15").join("/");

test("French inventory and values agree with the packaged distribution", { tag: "@unit" }, () => {
  const rows = readFileSync(`${__dirname}/../../data/letterdistributions/french.csv`,'utf8').trim().split('\n').map(line => line.split(','));
  expect(tileCounts('FRA24')).toEqual(rows.slice(1).map(row => Number(row[2])));
  expect(tileValues('FRA24')).toEqual(Object.fromEntries(rows.map(row => [row[0], Number(row[3])])));
  expect(vowelLetters('FRA24')).toBe('AEIOUY');
  const position = emptyPosition('FRA24');
  expect(inventory(position).unseenCount).toBe(102);
  position.racks = ['SSSSSS', ''];
  expect(parseCGP(toCGP(position)).lexicon).toBe('FRA24');
  expect(() => parseCGP(toCGP(position).replace('FRA24','CSW24'))).toThrow('Too many S');
  expect(() => parseCGP(`${empty} WW/ 0/0 0 -lex FRA24`)).toThrow('Too many W');
  expect(analysisCommands(parseCGP(`${empty} AEINRST/ 0/0 0 -lex FRA24`), 'sim', {seconds:1,threads:1,candidates:2,plies:1,ttMiB:16}).join('\n')).toContain('-lex FRA24');
});

test("French messages preserve parameters and language preferences", { tag: "@unit" }, () => {
  for (const [key, value] of Object.entries(french)) {
    expect(value.match(/\{\d+\}/g)?.sort() || [], key).toEqual(key.match(/\{\d+\}/g)?.sort() || []);
  }
  expect(translate('Player {0}',[2],'fr')).toBe('Joueur 2');
  expect(translate('{0} played',['Score'],'fr')).toBe('Score a joué');
  expect(preferredLanguage({getItem:()=>null},['fr-FR'])).toBe('fr');
  expect(preferredLanguage({getItem:()=> 'en'},['fr-FR'])).toBe('en');
  expect(preferredLanguage({getItem:()=> 'invalid'},['en-US'])).toBe('en');
  expect(preferredLanguage({getItem:()=> {throw Error();}},['fr-CA'])).toBe('fr');
});

async function chooseFrench(page) {
  await page.goto('/wasmentry/');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await page.locator('#cancel-new-game').click();
  await page.locator('#open-lexicon-settings').click();
  await page.locator('#lexicon').selectOption('FRA24');
  await page.locator('#back-to-game').click();
}

test("French UI switches without losing a position, translates results and persists", async ({page}) => {
  await chooseFrench(page);
  await openGameActions(page);
  await page.locator('#import').click();
  await page.locator('#cgp').fill(`${empty} KMQWXYZ/ 0/0 0 -lex FRA24`);
  await page.locator('#import-form button[type=submit]').click();
  await expect(page.locator('#rack-tiles sup')).toHaveText(['10','2','8','10','10','10','10']);
  await expect(page.locator('#bag-count')).toHaveText('88 in bag');
  await page.locator('#open-settings').click();
  await page.locator('#language').selectOption('fr');
  await expect(page.locator('html')).toHaveAttribute('lang','fr');
  await expect(page.locator('#settings-heading')).toHaveText('Paramètres');
  await page.locator('#back-to-game').click();
  await expect(page.locator('#bag-count')).toHaveText('88 dans le sac');
  await expect(page.locator('#rack')).toHaveValue('KMQWXYZ');
  await expect(page.locator('#current-lexicon')).toHaveText('FRA24');
  await page.locator('[data-mode=kibitz]').click();
  await expect(page.locator('#status')).toHaveText('Terminé',{timeout:30000});
  await expect(page.locator('#results thead')).toContainText('Reliquat');
  await expect(page.locator('#analysis-meta')).toContainText('Équité statique');
  await expect(page.locator('#error')).toBeHidden();
  await page.screenshot({path:test.info().outputPath('french-desktop.png'),fullPage:true});
  await page.locator('#open-settings').click();
  await page.locator('#language').selectOption('en');
  await page.locator('#back-to-game').click();
  await expect(page.locator('#results thead')).toContainText('Leave');
  await expect(page.locator('#analysis-meta')).toContainText('Static equity');
  await expect(page.locator('#status')).toHaveText('Complete');
  await expect(page.locator('#rack')).toHaveValue('KMQWXYZ');
  await page.locator('#open-settings').click();
  await page.locator('#language').selectOption('fr');
  await page.reload();
  await expect(page.locator('#new-game-heading')).toHaveText('Nouvelle partie');
  await expect(page.locator('#status')).toHaveText('Moteur prêt',{timeout:30000});
});

test("French engine imports and round-trips games, scores ODS 9 words, simulates and switches back to English", async ({page}) => {
  test.setTimeout(90000);
  await chooseFrench(page);
  const result = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const {parseCGP,analysisCommands,parseResults,inventory} = await import('./analysis-model.mjs');
    const {readGCG} = await import('./gcg-loader.mjs');
    const engine = new EngineClient();
    const logs=[];
    engine.addEventListener('message',({detail})=> {if(detail.type==='output')logs.push(detail.text);});
    try {
      await engine.prepare('FRA24');
      const text='#lexicon FRA24\n#player1 p1 Étienne\n#player2 p2 Chloé\n>p1: AEINQST 8H QAT +20 20\n';
      const prepared=await readGCG(null,text,'CSW24');
      const {game}=await engine.importGCG(prepared.text,prepared.lexicon);
      const loaded=(await engine.importGCG(game.gcg,'FRA24')).game;
      const position=parseCGP(`${Array(15).fill('15').join('/')} CDEIOVS/ 0/0 0 -lex FRA24`);
      await engine.run(analysisCommands(position,'kibitz',{threads:1,candidates:5,plies:1,seconds:1,ttMiB:16}));
      const moves=parseResults(logs.join('\n'),'kibitz');
      logs.length=0;
      await engine.run(analysisCommands(position,'sim',{threads:1,candidates:2,plies:1,seconds:1,ttMiB:16}));
      const sim=parseResults(logs.join('\n'),'sim');
      await engine.prepare('CSW24');
      logs.length=0;
      position.lexicon='CSW24';
      await engine.run(analysisCommands(position,'kibitz',{threads:1,candidates:5,plies:1,seconds:1,ttMiB:16}));
      const english=parseResults(logs.join('\n'),'kibitz');
      let linkError='';
      try {await engine.convertXS('encodePosition','test','FRA24');}catch(error){linkError=error.message;}
      return {game,loaded,moves,sim,english,linkError,unseen:inventory(parseCGP(game.positions.at(-1).cgp, 'FRA24')).unseenCount};
    } finally {engine.worker.terminate();}
  });
  expect(result.game.players).toEqual(['Étienne','Chloé']);
  expect(result.loaded.gcg).toBe(result.game.gcg);
  expect(result.game.positions.at(-1).cgp).toContain('0/20');
  expect(result.moves.some(row=>row.move.includes('COVID'))).toBe(true);
  expect(result.sim.some(row=>row.iterations>0)).toBe(true);
  expect(result.english.length).toBeGreaterThan(0);
  expect(result.linkError).toContain('French share links');
});


test("French new game deals 102 tiles and supports a narrow screen", async ({page}) => {
  await page.setViewportSize({width:390,height:844});
  await chooseFrench(page);
  await page.locator('#open-settings').click();
  await page.locator('#language').selectOption('fr');
  await page.locator('#back-to-game').click();
  await page.locator('#new-game').click();
  await page.locator('#game-time').selectOption('0');
  await page.locator('#start-game').click();
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  await expect(page.locator('#current-lexicon')).toHaveText('FRA24');
  await expect(page.locator('#rack-tiles button')).toHaveCount(7);
  await expect(page.locator('#bag-count')).toHaveText('88 dans le sac');
  await expect(page.locator('#error')).toBeHidden();
  await page.screenshot({path:test.info().outputPath('french-mobile.png'),fullPage:true});
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(true);
});

test("French word maps fall back without interrupting analysis", async ({page}) => {
  test.setTimeout(120000);
  await page.addInitScript(() => localStorage.setItem('magpie-wmp-source', 'build'));
  await chooseFrench(page);
  await page.locator('#open-analysis-settings').click();
  await expect(page.locator('#wmp-settings')).toBeHidden();
  await expect(page.getByRole('button', {name:'Cache FRA24 WMP', exact:true})).toHaveCount(0);
  await page.locator('#back-to-game').click();
  const result = await page.evaluate(async () => {
    const {EngineClient} = await import('./engine-client.mjs');
    const engine=new EngineClient();
    let output='';
    const warnings=[];
    engine.addEventListener('message',({detail})=>{
      if(detail.type==='output'&&detail.command==='generate') { output=detail.text; }
      if(detail.type==='wmp_cache_warning') { warnings.push(detail.text); }
    });
    try {
      await engine.prepare('FRA24');
      const position=`${Array(15).fill('15').join('/')} CDEIOVS/ 0/0 0`;
      await engine.run(['set -lex FRA24 -wmp false -numplays 100 -hr false',`cgp ${position}`,'generate']);
      const baseline=output;
      await engine.prepareWMP('FRA24','build',1,true);
      await engine.run(['set -wmp true',`cgp ${position}`,'generate']);
      return {baseline,withMap:output,cached:engine.wmpCached,unavailable:engine.wmpUnavailable,warnings};
    } finally {engine.worker.terminate();}
  });
  expect(result.withMap).toBe(result.baseline);
  expect(result.cached).toBe(false);
  expect(result.unavailable).toBe(true);
  expect(result.warnings).toEqual(["French word maps are unavailable in this browser build. Using standard move generation."]);
});
