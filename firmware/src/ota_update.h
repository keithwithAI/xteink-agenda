#pragma once

enum OtaResult { OTA_UP_TO_DATE, OTA_UPDATED, OTA_FAILED };

/// Check server for newer firmware. If found, download, flash, and reboot.
/// Shows progress on e-ink display during download.
/// Returns OTA_UP_TO_DATE if no update needed, OTA_FAILED on error.
OtaResult checkAndApplyOta();
