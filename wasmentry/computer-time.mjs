// Match PlayChooser's estimate of one play per eight unplayed tiles per player.
// Reserve extra time for the browser's clock tick, rendering and move commit.
export function computerSearchPlan({time, remaining, overtime, manageTime, seconds}, unplayedTiles, mode) {
  if (mode === 'kibitz') return {mode, seconds, useFullBudget:false};
  if (!time) return {mode, seconds, useFullBudget:false};
  const plays = Math.max(1, Math.ceil(unplayedTiles / 8));
  let window = remaining;
  if (remaining <= 0) {
    const used = -remaining;
    // Only spend the penalty minute already started (or the first at flag fall).
    window = used === 0 ? 60 : Math.ceil(used / 60) * 60 - used;
    if (overtime !== 'unlimited') window = Math.min(window, Math.max(0, Number(overtime) - used));
  }
  const reserve = Math.max(0.25, remaining <= 0 ? 0.6 : 0) + plays * 0.02;
  const available = Math.max(0, window - reserve);
  const budget = manageTime ? available / plays : Math.min(seconds, available);
  const minimum = mode === 'endgame' ? 0.25 : 0.05;
  if (budget < minimum) return {mode:'kibitz', seconds:0, useFullBudget:false};
  return {mode, seconds:budget, useFullBudget:!!manageTime};
}
