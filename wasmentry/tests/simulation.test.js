import { test, expect } from "@playwright/test";

test.describe("WASM Worker Simulation Tests", () => {
  test("should load WASM and run midgame simulation successfully", async ({
    page,
  }) => {
    // Set a longer timeout for this test since simulation takes time
    test.setTimeout(120000); // 2 minutes

    // Navigate to the test page
    await page.goto("/wasmentry/test-worker.html");

    // Wait for WASM to load - look for success status
    await expect(page.locator("#status")).toContainText(
      "WASM loaded and ready",
      {
        timeout: 30000,
      },
    );

    // Load sample midgame position
    await page.selectOption("#sampleSelect", "midgame");
    await page.click("#loadSampleButton");

    // Verify CGP input was populated
    const cgpInput = await page.inputValue("#inputParams");
    expect(cgpInput).toContain("EEEIILZ");
    expect(cgpInput).toContain("NWL23");

    // Clear output before running
    await page.evaluate(() => {
      document.getElementById("output").value = "";
    });

    // Click Run Simulation
    await page.click("#submitButton");

    // Wait for precaching to start
    await expect(page.locator("#status")).toContainText("Precaching files", {
      timeout: 10000,
    });

    // Wait for simulation to start
    await expect(page.locator("#status")).toContainText("Running simulation", {
      timeout: 30000,
    });

    // Check that output is being populated (indicates simulation is running)
    await expect(page.locator("#output")).not.toHaveValue("", {
      timeout: 20000,
    });

    // Wait for completion (this may take a while)
    await expect(page.locator("#status")).toContainText("Complete", {
      timeout: 90000,
    });

    // Verify output contains simulation results
    const output = await page.inputValue("#output");

    // Should contain move suggestions with rankings (e.g., "1:  14F ZI(N)E")
    expect(output).toMatch(/\d+:/); // Pattern like "1:", "2:", etc.
    expect(output).toContain("Showing"); // Should show "Showing X of Y simmed plays"
    expect(output).toContain("Finished");

    // Verify the top move is 14F ZI(N)E
    expect(output).toMatch(/^1:\s+14F ZI\(N\)E/m);

    // Should NOT contain errors
    expect(output).not.toContain("ERROR");
    expect(output).not.toContain("failed");

    console.log("✓ Simulation completed successfully");
  });

  test("should handle stop button correctly", async ({ page }) => {
    test.setTimeout(60000);

    await page.goto("/wasmentry/test-worker.html");

    // Wait for WASM to load
    await expect(page.locator("#status")).toContainText(
      "WASM loaded and ready",
      {
        timeout: 30000,
      },
    );

    // Load sample and start simulation
    await page.selectOption("#sampleSelect", "midgame");
    await page.click("#loadSampleButton");
    await page.click("#submitButton");

    // Wait for simulation to start
    await expect(page.locator("#status")).toContainText("Running simulation", {
      timeout: 30000,
    });

    // Wait a bit for simulation to actually start running
    await page.waitForTimeout(2000);

    // Click stop
    await page.click("#stopButton");

    // Verify it stopped (should see "Stopped" status eventually)
    await expect(page.locator("#status")).toContainText(/Complete|Stopped/, {
      timeout: 10000,
    });

    console.log("✓ Stop functionality works");
  });
});
