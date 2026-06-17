# Leaves & Equity

> **Status:** outline — prose to be written.
> **Sources:** `src/def/equity_defs.h`, `src/def/static_eval_defs.h`,
> `src/ent/klv.h`, `src/impl/rack_info_table_maker.c`, `src/impl/rack_list.c`
> **References:** [wolges](https://github.com/andy-k/wolges) (KWG/KLV). [KLV format](../formats/klv.md).

## Equity is millipoints

!!! warning "Units"
    `Equity` (`int32_t`) stores **millipoints**: 42 points is `42000`. Convert
    with `int_to_equity()` / `equity_to_int()` / `double_to_equity()`. This
    avoids floating point in the hot path while keeping sub-point resolution.

## Static evaluation

<!-- NOTE: static equity = move score + leave value, with penalties/constants
     from static_eval_defs.h (e.g. opening-square placement, non-outplay
     adjustments). State the formula. -->

$$
\text{equity}(m) = \text{score}(m) + \text{leave\_value}(\text{rack} \setminus m)
$$

## Leaves and the KLV

<!-- NOTE: a leave is the kept tiles; KLV pairs a KWG of leaves with leave-value
     and word-count arrays for O(1) lookup. -->

## The Rack Info Table (RIT)

<!-- NOTE: precomputed per-rack info — leave values for all subsets, best-leave
     by size, and playthrough bitvectors. Built by rack_info_table_maker. -->

## Superleave sampling

<!-- NOTE: rack_list samples high-equity leaves from historical games rather
     than enumerating every rack (infeasible for large lexicons). How the sample
     is built and how leave values are estimated from it. -->
