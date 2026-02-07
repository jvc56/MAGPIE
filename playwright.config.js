import { defineConfig, devices } from '@playwright/test';

export default defineConfig({
  testDir: './tests/wasm',
  fullyParallel: false,
  forbidOnly: !!process.env.CI,
  retries: process.env.CI ? 2 : 0,
  workers: 1,
  reporter: 'html',
  use: {
    baseURL: 'http://localhost:8000',
    trace: 'on-first-retry',
    screenshot: 'only-on-failure',
  },
  projects: [
    {
      name: 'chromium',
      use: {
        ...devices['Desktop Chrome'],
        // Required for SharedArrayBuffer/pthread support
        launchOptions: {
          args: [
            '--enable-features=SharedArrayBuffer',
          ],
        },
      },
    },
  ],
  webServer: {
    command: 'python3 -u cors_server.py',
    cwd: './wasmentry',
    url: 'http://localhost:8000',
    reuseExistingServer: !process.env.CI,
    timeout: 120000,
    stdout: 'pipe',
    stderr: 'pipe',
    env: {
      PYTHONUNBUFFERED: '1',
      EMSDK_QUIET: '1',
    },
  },
});
