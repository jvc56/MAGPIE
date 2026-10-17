export const GameAction = Object.freeze({
  IMPORT: 0, COMMIT: 1, NOTE: 2, CHALLENGE: 3, DEAL: 4, LIVE_MOVE: 5, LIVE_CHALLENGE: 6,
});

// Values match BrowserChallengeRule. Legacy saved sessions retain their rule.
export function challengeRuleCode(rule, points = 5, unit = "play") {
  if (rule === "single") return 1;
  if (rule === "double") return 2;
  if (rule === "five") return 3;
  if (rule === "ten") return 4;
  if (rule === "penalty") return (Number(points) === 10 ? 4 : 3) + (unit === "word" ? 2 : 0);
  return 0;
}
