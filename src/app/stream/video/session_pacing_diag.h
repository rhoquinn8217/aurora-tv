#pragma once

#include <Limelight.h>
#include "ss4s.h"

/**
 * Optional present-to-present pacing log (flag file or env).
 * Does not change Feed, PTS, codec, or bitrate.
 */
void session_pacing_diag_reset(void);

void session_pacing_diag_on_present(PDECODE_UNIT decodeUnit, SS4S_Player *player, int feed_ok);