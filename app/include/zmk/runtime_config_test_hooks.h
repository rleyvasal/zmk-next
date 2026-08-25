/*
 * Copyright (c) 2026 The ZMK Next Contributors
 * SPDX-License-Identifier: MIT
 *
 * Test-only. Not part of the public Runtime Config API (see
 * zmk/runtime_config.h). Only compiled when CONFIG_ZMK_RUNTIME_CONFIG_TEST_HOOKS
 * is enabled, which production builds must never do.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if IS_ENABLED(CONFIG_ZMK_RUNTIME_CONFIG_TEST_HOOKS)

/*
 * Persists the same way zmk_runtime_config_persist_update() does -
 * invalidate the inactive slot's manifest, then write its chunks in order,
 * then (normally) write its manifest last - except this stops after writing
 * exactly chunk_count chunks and, if write_manifest is false, never writes
 * the manifest at all. Lets a test construct an exact torn-write state:
 * zero chunks written, some-but-not-all chunks written, or every chunk
 * written but the manifest missing, without needing to actually interrupt
 * a real write in flight.
 */
int zmk_runtime_config_test_persist_truncated(uint32_t update_id, size_t chunk_count,
                                              bool write_manifest);

/*
 * Re-runs Settings load exactly as main() does once at real boot, against
 * whatever is currently in the backing store, after first resetting the
 * persistence layer's in-RAM state to zero the same way a real reboot
 * would. Lets one test process exercise multiple "boots" in sequence.
 */
void zmk_runtime_config_test_reload(void);

#endif /* CONFIG_ZMK_RUNTIME_CONFIG_TEST_HOOKS */
