import {test,expect} from '@playwright/test';
async function review(page) {
 await page.goto('/wasmentry/');
 await expect(page.locator('#start-game')).toBeEnabled({timeout:30000});
 await page.locator('[data-game-kind=record]').click(); await page.locator('#start-game').click();
 await page.locator('#rack').fill('AEINRST'); await page.locator('#rack').dispatchEvent('change');
}
test('manual plays are unioned with the configured top plays before any kibitz and on subsequent sims',async({page})=>{
 await review(page);
 await page.locator('#open-analysis-settings').click();
 await page.locator('#candidates').fill('3'); await page.locator('#seconds').fill('1');
 await page.locator('#wmp-source').selectOption('off'); await page.locator('#back-to-game').click();
 await page.locator('#board [data-row="7"][data-col="7"]').click(); await page.keyboard.type('at');
 await page.locator('#analyze-move').click(); await expect(page.locator('#status')).toHaveText('Play added');
 await expect(page.locator('#results tbody tr')).toHaveCount(4);
 await page.locator('[data-mode=sim]').click(); await expect(page.locator('#stop')).toBeDisabled();
 await expect(page.locator('#results tbody tr')).toHaveCount(4);
 await expect(page.locator('#results button').filter({hasText:'8H AT'})).toHaveCount(1);
 await page.locator('#open-analysis-settings').click(); await page.locator('#candidates').fill('5'); await page.locator('#back-to-game').click();
 await page.locator('[data-mode=sim]').click(); await expect(page.locator('#stop')).toBeDisabled();
 await expect(page.locator('#results tbody tr')).toHaveCount(6);
 await expect(page.locator('#results button').filter({hasText:'8H AT'})).toHaveCount(1);
});
test('sidebars contain game controls and bag count; equal-height panels show exactly fifteen plays',async({page})=>{
 await page.setViewportSize({width:1936,height:1080}); await review(page);
 await expect(page.locator('#game-header-actions #new-game')).toBeVisible();
 await expect(page.locator('#game-header-actions [aria-label=GCG]')).toBeVisible();
 await expect(page.locator('#game-header-actions [aria-label=CGP]')).toBeVisible();
 await expect(page.locator('#brand-rail #bag-count')).toBeVisible();
 await expect(page.locator('.analysis h2')).toHaveCount(0);
 if (await page.locator('#position-editor').getAttribute('open') !== null) await page.locator('#position-editor summary').click();
 await page.locator('[data-mode=kibitz]').click(); await expect(page.locator('#stop')).toBeDisabled();
 const dimensions=await page.evaluate(()=>{
  const area=document.querySelector('.results-scroll').getBoundingClientRect();
  const rows=[...document.querySelectorAll('#results tbody tr')].map(row=>row.getBoundingClientRect());
  return {visible:rows.filter(row=>row.top>=area.top&&row.bottom<=area.bottom+0.5).length,
   board:document.querySelector('.position').getBoundingClientRect().height,
   analysis:document.querySelector('.analysis').getBoundingClientRect().height};
 });
 expect(dimensions.visible).toBe(15); expect(Math.abs(dimensions.board-dimensions.analysis)).toBeLessThan(1);
 await expect(page.locator('#load-gcg')).toBeVisible();
 await page.screenshot({path:test.info().outputPath('sidebar-panels.png')});
});
