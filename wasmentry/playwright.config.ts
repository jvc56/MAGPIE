import { defineConfig, devices } from "@playwright/test";

const port = Number(process.env.MAGPIE_TEST_PORT || 8000);
const staticPort = Number(process.env.MAGPIE_STATIC_TEST_PORT || 8001);

export default defineConfig({
  testDir: "./tests",
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  workers: 1,
  reporter: "html",
  use: {
    baseURL: `http://127.0.0.1:${port}`,
    trace: "on-first-retry",
    screenshot: "only-on-failure",
  },
  projects: [
    { name: "chromium", use: { ...devices["Desktop Chrome"] } },
    { name: "webkit", testMatch: "**/engine-compat.test.js", use: { ...devices["Desktop Safari"] } },
    {
      name: "static-host",
      testMatch: "**/analysis.test.js",
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
