// The allocator probe runs immediately before PEG, after maps and the position
// are prepared. Device type and physical-RAM hints do not determine this cap.
export function pegThreadsForMemory(requested, fits) {
  if (!Number.isInteger(requested) || requested < 1 || requested > 32) {
    throw new Error("PEG threads must be between 1 and 32.");
  }
  for (const count of [...new Set([requested, Math.min(requested, 2), 1])]) {
    if (fits(count)) return count;
  }
  throw new Error("Not enough WASM memory to start preendgame analysis. Free word maps or reload and try again.");
}
