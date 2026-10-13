// Device hints choose conservative defaults, not a claim about available RAM.
// iPadOS can identify as a Mac, so include touch-capable Macs.
export function deviceBudget(device = navigator) {
  const mobile = device.userAgentData?.mobile === true ||
    /Android|iPhone|iPad|iPod/i.test(device.userAgent || "") ||
    (/Mac/i.test(device.platform || "") && device.maxTouchPoints > 1);
  const cores = Math.max(1, Number(device.hardwareConcurrency) || 2);
  const constrained = mobile && (!device.deviceMemory || device.deviceMemory <= 4);
  return {
    mobile,
    threads: Math.min(constrained ? 2 : 4, Math.max(1, cores - 1)),
    maxThreads: Math.min(mobile ? 4 : 32, cores),
    ttMiB: constrained ? 16 : 32,
  };
}

// Preserve the saved preference; release installed maps only for late-game
// searches on mobile. Earlier positions can load them again from disk cache.
export function searchWordMapSource(source, bagCount, budget) {
  return budget.mobile && bagCount <= 4 ? "off" : source;
}
