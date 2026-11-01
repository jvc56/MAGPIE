#ifndef PEG_NET_EVAL_TEST_H
#define PEG_NET_EVAL_TEST_H

// pegnet: modes that evaluate the nets: picks against exact continuations, game
// pairs, simulations and fixed time budgets.

#include "../src/util/string_util.h"

// pegnet:peg1play
// Plays requested root candidates with the stored game's actual last tile,
// then a common continuation (0 = static top1, 2 = fixed two-ply endgame)
// to game over. Choices CSV: root,candidate (static rank), one per line.
void peg1_play_mode(const StringSplitter *fields);

// pegnet:peg1util
// Evaluates a v3 chooser on stored or fresh roots: decisions.csv (with the
// chosen move's estimated win probability and spread), choices.csv for
// peg1play and inputs.f32 for parity. Fields: first model, second model or
// "-", shortlist, w_spread (negative = margin mode), scale, positions
// prefix, output prefix, check.
void peg1_utility_net_eval_mode(const StringSplitter *fields);

// pegnet:peg1games3
// Full-game pairs with the v3 networks. Each game is static self-play from
// the opening (the peg1data source procedure, without rejection). If a 1PEG
// arises, the mover's choice is made twice -- static top-1 and the v3
// chooser -- and each branch is continued to game over with the common
// continuation (0 = static, 2 = fixed two-ply endgame); both final spreads
// come from the 1PEG mover's view. Games without a 1PEG are recorded as
// identical pairs. Fields: first model, second model or "-", shortlist,
// w_spread (negative = margin mode), scale, count, offset, seed, depth 0|2,
// prefix.
void peg1_game_pairs_mode(const StringSplitter *fields);

// pegnet:peg1greedy
// PEG's greedy seed pick on stored 1PEG roots, scored with the common
// continuations (0 = static top 1, 2 = fixed two-ply endgame). Fields:
// positions prefix, output prefix, check. Writes PREFIX.greedy.csv.
void peg1_greedy_seed_eval_mode(const StringSplitter *fields);

// pegnet:peg1exact
// Scores per-board picks with the exact continuation. Choices CSV rows:
// root,code where code >= 0 is a static rank in the top-64 placement pool,
// -1 the PEG greedy seed's pick over every move and -2 MAGPIE's default PEG
// pick (both recomputed here from public information). Output rows are
// appended and fsynced per board; a rerun skips (root, code) pairs already
// in the output.
void peg1_exact_eval_mode(const StringSplitter *fields);

// pegnet:peg1pred
// The v3 networks' head outputs for every scored candidate of stored 1PEG
// roots: estimated win probability and spread of each static top-K
// placement, from each network on its own (no cascade). Fields: fast model,
// strong model, positions prefix, output prefix, max rank. Writes
// PREFIX.pred.csv: root, rank, candidates, tiles, each network's win and
// spread (empty past its top K), quoted move.
void peg1_utility_net_predict_mode(const StringSplitter *fields);

// pegnet:peg2eval
// 2PEG picks scored in the actual game: the stored board's bag tiles, drawn in
// a fixed per-board order, then the reply policy for every one-tile-bag turn
// and an exact endgame solve. Codes: >= 0 static rank; -2 MAGPIE's default
// PEG; -4 default PEG with the network replacing the nested 1PEG; -5 the same
// with information-rule checks (network POV at every nested call, and the
// whole pick under a hidden-tile swap). Fields: positions prefix, choices CSV,
// output prefix, reply (net|peg), plies, TT fraction, first, second|-,
// shortlist, w_spread, scale.
void peg2_exact_eval_mode(const StringSplitter *fields);

// pegnet:peg2pick
// Fields: positions prefix, output prefix, model, first model|-, stage N,
// top K (0 = the model's), check boards, dump world rows (0|1). Writes
// OUT.picks.csv (pick and timing per board), OUT.scores.csv and, with dump,
// OUT.worlds.f32. On the first check boards the pick is recomputed with the
// bag swapped for two other unseen tiles (from the opponent's rack) and must
// be identical, scores included.
void peg2_pick_eval_mode(const StringSplitter *fields);

// pegnet:peg2wide
// Fields: positions prefix, output prefix, first model, full model|-, stage
// N, check boards, dump (0 = scored rows only, 1 = every placement). Writes
// OUT.picks.csv (the pick as a wide rank, and timing) and OUT.ranks.csv
// (root, wide rank, tiles, static equity, first score, full score or NAN,
// move). On the first check boards the whole pick is recomputed with the bag
// swapped for two other unseen tiles and must be identical, scores included.
void peg2_wide_pick_eval_mode(const StringSplitter *fields);

// pegnet:peg3eval
// Exact continuation of 3PEG picks in the actual world: the pick is played
// from the root (draws come from the root's bag in its stored order, the same
// for every code), the drawing turns are played by peg3_play_drawing_turns and
// the endgame is solved exactly (as in peg2eval). Fields: positions prefix,
// choices (root,code rows), output prefix, reply (net|peg), plies, tt
// fraction, 1PEG first, second|-, shortlist, w_spread, scale, 2PEG model,
// 2PEG first model|-, stage N, top K (0 = the model's). Codes: >= 0 a static
// rank; -2 default PEG; -3 default PEG over every placement but not the pass;
// -4 PEG with the 1PEG network at one-tile nested nodes; -7 PEG with both
// choosers; -8 = -7 with every hidden-tile check, including the root pick
// recomputed with one bag tile swapped for a tile of the opponent's rack.
// Codes -3, -4, -7 and -8 never pass at the root (PegArgs.only_moves = every
// placement): a root pass leaves the opponent a 3PEG, which no chooser covers,
// so PEG solves it with the full staged inner peg. PEG3_EVAL_NEST_DEPTH sets
// PegArgs.nested_max_depth for -4, -7 and -8 (default
// PEG_NESTED_DEFAULT_DEPTH); PEG2_PICK_MAX_STAGE and PEG2_PICK_BUDGET_S apply
// to every PEG pick with two or more tiles in the bag, PEG3_EVAL_THREADS to the
// root picks.
void peg3_exact_eval_mode(const StringSplitter *fields);

// pegnet:pegsim
// Late-game sims (PEG_SIM_BAG tiles in the bag at each root, default 7). Each
// root's static top-N tile placements (no exchanges, no pass) are simmed with
// plies-ply rollouts by round robin, with the same seed in every arm
// (PEG_SIM_SEED + root, so iteration k of a candidate is the same world in
// every arm), no inference, no bag cycling, rollout PAT off and utility
// (1, 0.5, 100). Arms (PEG_SIM_ARMS, default "0,1,2"): 0 static rollouts,
// iterations per candidate; 1 the same with the nets at 1 and 2 tiles in the
// bag (peg_sim_rollout_move); 2 static rollouts for arm 1's wall time on
// this root, no iteration cap (needs arm 1); 3 arm 1 with every bag-empty ply
// chosen by a PEG_SIM_EG_PLIES-ply endgame search (default 2; 0 = the
// solver's greedy playout move); 4 static rollouts with that endgame
// search; 5 static rollouts for arm 3's wall time (needs arm 3). The
// choosers' pick caches are cleared before every arm, so each arm's time
// includes its own chooser work. PEG_SIM_THREADS threads
// (default 1). PEG_SIM_CHECK=1 turns on the choosers' hidden-tile checks (each
// rollout decision recomputed with a hidden tile swapped; fatal on a
// difference). Fields: positions prefix, output prefix, candidates, iterations
// per candidate, plies, 1PEG first, second|-, shortlist, w_spread, scale, 2PEG
// model, 2PEG first|-, stage N, top K. Writes PREFIX.sim.csv (per root and arm:
// the chosen candidate's static rank and its utility, win% and equity means,
// the sim's iterations and ms, the choosers' calls and ms in this sim; quoted
// move) and PREFIX.cands.csv (every candidate's samples and means per arm).
// Resumes by root.
void peg_sim_mode(const StringSplitter *fields);

// pegnet:pegsimbai
// MAGPIE's BAI replayed on a sim's recorded rollouts (PEG_SIM_SAMPLES): per
// root, arm PEG_SIM_BAI_ARM's (default 3) rollouts become a table of random
// variables (RANDOM_VARIABLES_TABLE: candidate c's n-th sample is its n-th
// rollout in seed order, the same world for every candidate; past the last,
// they start over; with PEG_SIM_BAI_STREAM=SEED, in the order a live sim with
// PEG_SIM_SEED=SEED draws their seeds instead), and each scheme's BAI runs on
// one thread with the GK16 threshold. Fields: samples file, output CSV, then
// schemes NAME=RULE,SCOND,MIN,LIMIT[,elim][,preK][,zZ][,capN] (rule rr or tt,
// the stopping confidence in percent, iterations per candidate first, the cap
// on all rollouts; elim sets BAIOptions.eliminate; preK limits BAI to the K
// candidates arm 0's recorded rollouts rank best, as PEG_SIM_PREFILTER does;
// zZ eliminates at Z standard errors, BAIOptions.elimination_z; capN gives no
// candidate more than N, BAIOptions.arm_sample_limit).
// Writes per root and scheme: the prefilter, BAI's pick (static rank), its
// status, the rollouts and each candidate's rollouts (semicolon-separated, by
// static rank).
void peg_sim_bai_replay_mode(const StringSplitter *fields);

// pegnet:pegbudget
// Matched-budget 2PEG picks (pegbudget/PROTOCOL-ENGINE.txt). Fields:
// positions prefix (2 tiles in the bag), output prefix, budgets in seconds
// (comma-separated), 1PEG first, second|-, shortlist, w_spread, scale, 2PEG
// model, 2PEG first|-, stage N, top K. PEG_BUDGET_THREADS (default 10) threads
// per PEG solve; PEG_BUDGET_ARMS a subset of
// "net,peg,pegnet,order,hybrid,hybridu,h2peg" (default
// "net,peg,pegnet,order").
// Writes PREFIX.pegb.csv: root, code, arm, budget_s, ms, net_ms, stage,
// partial, stage1_done, s1_ms (when the first halving stage began, -1 = never),
// quoted move (a peg3oracle picks file). Roots already in the output are
// skipped.
void peg_budget_mode(const StringSplitter *fields);

// pegnet:pegrank
// Where each pick of a pegbudget run falls in the candidate orders. Fields:
// positions prefix, pegb csv, output path, 2PEG model, 2PEG first|-, stage N,
// top K. Writes root, code, net_rank (the 2PEG chooser's order over the
// static top 64; -1 = outside it) and equity_rank (every placement in equity
// order; -1 = not found), both 0-based.
void peg_budget_pick_ranks_mode(const StringSplitter *fields);

#endif
