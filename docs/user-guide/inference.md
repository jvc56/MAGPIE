# Inference

> **Status:** outline — prose to be written.
> **Sources:** `src/impl/inference.c`
> **Deep dive:** [Inference](../algorithms/inference.md)

## What inference answers

<!-- NOTE: given the opponent's observed play(s), what racks are consistent with
     them having played that and not something better? Narrows the opponent's
     likely tiles. -->

## Running it

<!-- NOTE: `infer` at a position; typical flow is `goto <n>` then `infer`. -->

## Reading the output

<!-- NOTE: how likely-rack candidates / tile probabilities are presented. -->

## Caveats

<!-- NOTE: assumes a rational opponent; sensitivity to the equity model. -->
