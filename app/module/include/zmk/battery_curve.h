/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Battery level from a single-cell LiPo's voltage, following its discharge curve rather than a
 * straight line. The cell spends most of its charge between about 3.7 and 3.9 V and falls
 * quickly below 3.65 V, so a straight line from 3.45 V to 4.2 V reads high near empty: 3.61 V
 * is 21% on that line and 5% here. Entry i is the voltage at i * 5 percent, from a typical
 * resting LiPo chart; levels in between are interpolated. */
static inline uint8_t zmk_battery_lipo_mv_to_pct(int16_t bat_mv) {
    static const int16_t mv_at_5pct[] = {
        3270, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820, 3840,
        3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200,
    };
    const int last = (int)(sizeof(mv_at_5pct) / sizeof(mv_at_5pct[0])) - 1;

    if (bat_mv <= mv_at_5pct[0]) {
        return 0;
    }
    if (bat_mv >= mv_at_5pct[last]) {
        return 100;
    }
    for (int i = 1; i <= last; i++) {
        if (bat_mv < mv_at_5pct[i]) {
            int low = mv_at_5pct[i - 1];
            return (uint8_t)((i - 1) * 5 + (bat_mv - low) * 5 / (mv_at_5pct[i] - low));
        }
    }
    return 100;
}
