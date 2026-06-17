# Inference

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/inference.c`
> **User guide:** [Inference](../user-guide/inference.md)

## The question

<!-- NOTE: given an opponent's observed play, which racks would have led a
     rational player to make exactly that play? Use this to estimate the
     distribution over the opponent's remaining tiles. -->

## Method

<!-- NOTE: enumerate racks consistent with the unseen tiles; for each, check
     whether the observed play is (near) best by equity. Keep those where it is;
     weight by draw probability. Describe the threshold/tolerance. -->

## Output

<!-- NOTE: the resulting probability mass over candidate racks / individual
     tiles. -->

## Assumptions & limits

<!-- NOTE: rationality assumption, dependence on the equity model, combinatorial
     cost and how it's bounded. -->
