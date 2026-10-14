import { defineConfig, devices } from "@playwright/test";

const port = Number(process.env.MAGPIE_TEST_PORT || 8000);
const staticPort = Number(process.env.MAGPIE_STATIC_TEST_PORT || 8001);

export default defineConfig({
  testDir: "./tests",
  // One engine at a time per runner; CI shards distribute cases across runners.
  fullyParallel: true,
  forbidOnly: !!process.env.CI,
  retries: 0,
  workers: 1,
  reporter: process.env.CI ? [["line"], ["html", { open: "never" }]] : "html",
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    trace: "retain-on-failure",
    screenshot: "only-on-failure",
  },
  projects: [
    { name: "unit", grep: /@unit/ },
    { name: "chromium", grepInvert: /@unit/, use: { ...devices["Desktop Chrome"] } },
    {
      name: "firefox",
      grepInvert: /@unit/,
      use: { ...devices["Desktop Firefox"] },
    },
    ...(process.env.MAGPIE_TEST_EDGE ? [{
      name: "edge",
      // Edge shares Chromium's engine; check branded-browser startup and searches.
      testMatch: "**/analysis.test.js",
      grep: /all four modes use the real WASM engine/,
      use: {
        ...devices["Desktop Edge"],
        channel: "msedge",
        launchOptions: { executablePath: process.env.MAGPIE_EDGE_PATH },
      },
    }] : []),
    {
      name: "webkit",
      // Linux WebKit occasionally stalls before page/worker startup completes.
      // A failed CI attempt is retained in the report; Playwright retries once
      // in a new worker/browser process, with the same timeouts and assertions.
      retries: process.env.CI ? 1 : 0,
      grepInvert: /@unit/,
      use: { ...devices["Desktop Safari"] },
    },
    {
      name: "static-host",
      testMatch: ["**/analysis.test.js", "**/isolation.test.js", "**/xs-links.test.js"],
      grep: /all four modes use the real WASM engine|plain static hosting starts|plain static hosting opens shared links/,
      use: { ...devices["Desktop Chrome"], baseURL: `http://127.0.0.1:${staticPort}` },
    },
  ],
  webServer: [
    {
      command: `python3 -u cors_server.py ${port}`,
      url: `http://127.0.0.1:${port}`,
      reuseExistingServer: !process.env.CI,
      timeout: 120000,
      stdout: "pipe",
      stderr: "pipe",
      env: { PYTHONUNBUFFERED: "1", EMSDK_QUIET: "1" },
    },
    {
      command: `python3 -m http.server ${staticPort} --bind 127.0.0.1 --directory ..`,
      url: `http://127.0.0.1:${staticPort}`,
      reuseExistingServer: !process.env.CI,
    },
  ],
});
