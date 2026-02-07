import { test, expect } from '@playwright/test';

test.describe('WASM Worker Simulation Tests', () => {
  test('should load WASM and run midgame simulation successfully', async ({ page }) => {
    // Set a longer timeout for this test since simulation takes time
    test.setTimeout(120000); // 2 minutes

    // Navigate to the test page
    await page.goto('/test-worker.html');

    // Wait for WASM to load - look for success status
    await expect(page.locator('#status')).toContainText('WASM loaded and ready', {
      timeout: 30000,
    });

    // Load sample midgame position
    await page.selectOption('#sampleSelect', 'midgame');
    await page.click('#loadSampleButton');

    // Verify CGP input was populated
    const cgpInput = await page.inputValue('#inputParams');
    expect(cgpInput).toContain('EEEIILZ');
    expect(cgpInput).toContain('NWL23');

    // Clear output before running
    await page.evaluate(() => {
      document.getElementById('output').value = '';
    });

    // Click Run Simulation
    await page.click('#submitButton');

    // Wait for precaching to start
    await expect(page.locator('#status')).toContainText('Precaching files', {
      timeout: 10000,
    });

    // Wait for simulation to start
    await expect(page.locator('#status')).toContainText('Running simulation', {
      timeout: 30000,
    });

    // Check that output shows thread status changing (indicates workers are running)
    // The output should show "thread status: 1" when threads are active
    await expect(page.locator('#output')).toContainText('thread status: 1', {
      timeout: 20000,
    });

    // Wait for completion (this may take a while)
    await expect(page.locator('#status')).toContainText('Complete', {
      timeout: 90000,
    });

    // Verify output contains simulation results
    const output = await page.inputValue('#output');

    // Should contain move suggestions and equity values
    expect(output).toMatch(/\d+\.\s+\w+/); // Pattern like "1. WORD"
    expect(output).toMatch(/[\d.]+/); // Some numeric values (equity/scores)

    // Should NOT contain errors
    expect(output).not.toContain('ERROR');
    expect(output).not.toContain('failed');

    console.log('✓ Simulation completed successfully');
  });

  test('should handle stop button correctly', async ({ page }) => {
    test.setTimeout(60000);

    await page.goto('/test-worker.html');

    // Wait for WASM to load
    await expect(page.locator('#status')).toContainText('WASM loaded and ready', {
      timeout: 30000,
    });

    // Load sample and start simulation
    await page.selectOption('#sampleSelect', 'midgame');
    await page.click('#loadSampleButton');
    await page.click('#submitButton');

    // Wait for simulation to start
    await expect(page.locator('#status')).toContainText('Running simulation', {
      timeout: 30000,
    });

    // Wait a bit for simulation to actually start running
    await page.waitForTimeout(2000);

    // Click stop
    await page.click('#stopButton');

    // Verify it stopped (should see "Stopped" status eventually)
    await expect(page.locator('#status')).toContainText(/Complete|Stopped/, {
      timeout: 10000,
    });

    console.log('✓ Stop functionality works');
  });
});
