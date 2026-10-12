// Estimate one play per eight unplayed tiles per player. Ordinary computer
// simulations stay brief; save the clock for preendgame and endgame searches.
const SIM_SECONDS = 10;
// Reserve extra time for the browser's clock tick, rendering and move commit.
export function computerSearchPlan({time, remaining, overtime, manageTime, seconds, penalty}, unplayedTiles, mode) {
  if (mode === 'kibitz') return {mode, seconds, useFullBudget:false};
  const moveLimit = mode === 'sim' ? Math.min(seconds, SIM_SECONDS) : seconds;
  if (!time) return {mode, seconds:moveLimit, useFullBudget:false};
  const plays = Math.max(1, Math.ceil(unplayedTiles / 8));
  let window = remaining;
  if (remaining <= 0) {
    const used = -remaining;
    // Only spend the penalty minute already started (or the first at flag fall).
    window = penalty === "second" ? 0 : used === 0 ? 60 : Math.ceil(used / 60) * 60 - used;
    if (overtime !== 'unlimited') window = Math.min(window, Math.max(0, Number(overtime) - used));
  }
  const reserve = Math.max(0.25, remaining <= 0 ? 0.6 : 0) + plays * 0.02;
  const available = Math.max(0, window - reserve);
  const automaticBudget = mode === 'sim'
    ? Math.min(SIM_SECONDS, available / plays)
    // Leave a short turn budget for the moves after this solve. On a short
    // clock, share what remains rather than reserving more than we have.
    : mode === 'endgame' ? available
    : Math.max(available / plays, available - SIM_SECONDS * (plays - 1));
  const budget = manageTime ? automaticBudget : Math.min(moveLimit, available);
  const minimum = mode === 'endgame' ? 0.25 : 0.05;
  if (budget < minimum) return {mode:'kibitz', seconds:0, useFullBudget:false};
  return {mode, seconds:budget, useFullBudget:false};
}
