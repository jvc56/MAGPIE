// Analysis and interaction tests explicitly choose their fixture; real startup
// offers New game and has no example tiles. Startup itself has separate coverage.
export async function openSample(page) {
  await page.locator('#cancel-new-game').click();
  await page.locator('#position-editor > summary').click();
  await page.locator('#sample').selectOption('opening');
  await page.locator('#position-editor > summary').click();
}
