#!/usr/bin/env python3
"""Small-sample inference helpers for source-game-clustered summaries."""

from __future__ import annotations

import math


def _beta_continued_fraction(a: float, b: float, x: float) -> float:
    maximum_iterations = 200
    epsilon = 3.0e-14
    minimum = 1.0e-300
    qab = a + b
    qap = a + 1.0
    qam = a - 1.0
    c = 1.0
    d = 1.0 - qab * x / qap
    if abs(d) < minimum:
        d = minimum
    d = 1.0 / d
    result = d
    for iteration in range(1, maximum_iterations + 1):
        even = 2 * iteration
        coefficient = (
            iteration * (b - iteration) * x
            / ((qam + even) * (a + even))
        )
        d = 1.0 + coefficient * d
        if abs(d) < minimum:
            d = minimum
        c = 1.0 + coefficient / c
        if abs(c) < minimum:
            c = minimum
        d = 1.0 / d
        result *= d * c

        coefficient = -(
            (a + iteration)
            * (qab + iteration)
            * x
            / ((a + even) * (qap + even))
        )
        d = 1.0 + coefficient * d
        if abs(d) < minimum:
            d = minimum
        c = 1.0 + coefficient / c
        if abs(c) < minimum:
            c = minimum
        d = 1.0 / d
        change = d * c
        result *= change
        if abs(change - 1.0) <= epsilon:
            return result
    raise ArithmeticError("incomplete beta continued fraction did not converge")


def regularized_incomplete_beta(a: float, b: float, x: float) -> float:
    if a <= 0.0 or b <= 0.0 or not 0.0 <= x <= 1.0:
        raise ValueError("invalid regularized incomplete beta arguments")
    if x == 0.0:
        return 0.0
    if x == 1.0:
        return 1.0
    scale = math.exp(
        math.lgamma(a + b)
        - math.lgamma(a)
        - math.lgamma(b)
        + a * math.log(x)
        + b * math.log1p(-x)
    )
    if x < (a + 1.0) / (a + b + 2.0):
        return scale * _beta_continued_fraction(a, b, x) / a
    return 1.0 - (
        scale * _beta_continued_fraction(b, a, 1.0 - x) / b
    )


def student_t_two_sided_p_value(statistic: float, degrees_freedom: int) -> float:
    if degrees_freedom <= 0 or not math.isfinite(statistic):
        raise ValueError("Student-t statistic and degrees of freedom are invalid")
    magnitude = abs(statistic)
    x = degrees_freedom / (degrees_freedom + magnitude * magnitude)
    return regularized_incomplete_beta(degrees_freedom / 2.0, 0.5, x)


def student_t_critical(confidence: float, degrees_freedom: int) -> float:
    if not 0.0 < confidence < 1.0 or degrees_freedom <= 0:
        raise ValueError("Student-t confidence and degrees of freedom are invalid")
    alpha = 1.0 - confidence
    lower = 0.0
    upper = 1.0
    while student_t_two_sided_p_value(upper, degrees_freedom) > alpha:
        upper *= 2.0
    for _ in range(80):
        midpoint = (lower + upper) / 2.0
        if student_t_two_sided_p_value(midpoint, degrees_freedom) > alpha:
            lower = midpoint
        else:
            upper = midpoint
    return (lower + upper) / 2.0
