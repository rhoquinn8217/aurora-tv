#include <stdio.h>
#include <stdlib.h>
#include "unity.h"
#include "app_settings.h"
#include "uuidstr.h"

#ifndef FIXTURES_PATH_PREFIX
#define FIXTURES_PATH_PREFIX "./"
#endif

app_settings_t settings;

void setUp() {
    char *dir = malloc(128);
    uuidstr_t uuid;
    uuidstr_random(&uuid);
    snprintf(dir, 128, "/tmp/moonlight-%s", (char *) &uuid);
    settings_initialize(&settings, dir);
}

void tearDown() {
    settings_clear(&settings);
    free(settings.conf_dir);
}

void testReadINI() {
    char *ini_backup = settings.ini_path;
    settings.ini_path = FIXTURES_PATH_PREFIX "settings_read.ini";
    TEST_ASSERT_TRUE(settings_read(&settings));
    settings.ini_path = ini_backup;
    TEST_ASSERT_EQUAL_INT(TOUCHPAD_MODE_MOUSE, settings.touchpad_mode);
    TEST_ASSERT_EQUAL_INT(125, settings.touchpad_speed);
    TEST_ASSERT_FALSE(settings.touchpad_multitouch);
    TEST_ASSERT_FALSE(settings.touchpad_natural_scroll);
}

void testWriteINI() {
    settings.touchpad_mode = TOUCHPAD_MODE_MOUSE;
    settings.touchpad_speed = 175;
    settings.touchpad_multitouch = false;
    settings.touchpad_natural_scroll = false;

    char *ini_backup = settings.ini_path;
    settings.ini_path = "settings_write_tmp.ini";
    TEST_ASSERT_TRUE(settings_save(&settings));
    settings.ini_path = ini_backup;

    app_settings_t loaded;
    settings_initialize(&loaded, settings.conf_dir);
    char *loaded_ini_backup = loaded.ini_path;
    loaded.ini_path = "settings_write_tmp.ini";
    TEST_ASSERT_TRUE(settings_read(&loaded));
    loaded.ini_path = loaded_ini_backup;

    TEST_ASSERT_EQUAL_INT(TOUCHPAD_MODE_MOUSE, loaded.touchpad_mode);
    TEST_ASSERT_EQUAL_INT(175, loaded.touchpad_speed);
    TEST_ASSERT_FALSE(loaded.touchpad_multitouch);
    TEST_ASSERT_FALSE(loaded.touchpad_natural_scroll);

    settings_clear(&loaded);
    remove("settings_write_tmp.ini");
}

void testControllerTouchpadDefaults() {
    TEST_ASSERT_EQUAL_INT(TOUCHPAD_MODE_NATIVE, settings.touchpad_mode);
    TEST_ASSERT_EQUAL_INT(TOUCHPAD_SPEED_DEFAULT, settings.touchpad_speed);
    TEST_ASSERT_TRUE(settings.touchpad_multitouch);
    TEST_ASSERT_TRUE(settings.touchpad_natural_scroll);
}

void testControllerTouchpadUnknownMode() {
    /* Anything we don't recognise -- including "off", a third mode that early
     * revisions of this feature had and that never shipped -- has to land on the
     * default rather than on whatever happened to be in the struct. */
    FILE *fp = fopen("settings_unknown_tmp.ini", "w");
    TEST_ASSERT_NOT_NULL(fp);
    fputs("[input]\ntouchpad_mode = off\n", fp);
    fclose(fp);

    app_settings_t unknown;
    settings_initialize(&unknown, settings.conf_dir);
    unknown.touchpad_mode = TOUCHPAD_MODE_MOUSE;
    char *unknown_ini_backup = unknown.ini_path;
    unknown.ini_path = "settings_unknown_tmp.ini";
    TEST_ASSERT_TRUE(settings_read(&unknown));
    unknown.ini_path = unknown_ini_backup;

    TEST_ASSERT_EQUAL_INT(TOUCHPAD_MODE_NATIVE, unknown.touchpad_mode);

    settings_clear(&unknown);
    remove("settings_unknown_tmp.ini");
}

void testSyncRefreshRate() {
#if defined(TARGET_WEBOS)
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);
    settings.client_refresh_rate_x100 = 0;
    settings.stream.fps = 60;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(60, settings.stream.fps);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);
#else
    settings.stream.fps = 119;
    settings.client_refresh_rate_x100 = 11988;
    settings_sync_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(120, settings.stream.fps);
#endif
}

void testNtscRefreshRateMapping() {
    TEST_ASSERT_EQUAL_INT(2997, settings_ntsc_refresh_rate_x100_for_fps(30));
    TEST_ASSERT_EQUAL_INT(5994, settings_ntsc_refresh_rate_x100_for_fps(60));
    TEST_ASSERT_EQUAL_INT(11988, settings_ntsc_refresh_rate_x100_for_fps(120));
    TEST_ASSERT_EQUAL_INT(23976, settings_ntsc_refresh_rate_x100_for_fps(240));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(90));
    TEST_ASSERT_EQUAL_INT(0, settings_ntsc_refresh_rate_x100_for_fps(144));
}

void testReconcileRefreshRate() {
    settings.stream.fps = 144;
    settings.client_refresh_rate_x100 = 11988;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
    TEST_ASSERT_EQUAL_INT(144, settings.stream.fps);

#if defined(TARGET_WEBOS)
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 0;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(5994, settings.client_refresh_rate_x100);

    /* NTSC off → integer presets clear fractional rate */
    settings.use_ntsc_refresh = false;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11988;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);

    settings.stream.fps = 60;
    settings.client_refresh_rate_x100 = 5994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);

    /* Custom fractional (not auto-NTSC) preserved with NTSC off */
    settings.use_ntsc_refresh = false;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11994, settings.client_refresh_rate_x100);

    /* Custom preserved with NTSC on */
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_reconcile_refresh_rate(&settings);
    TEST_ASSERT_EQUAL_INT(11994, settings.client_refresh_rate_x100);

    /* Preset apply forces NTSC or integer */
    settings.use_ntsc_refresh = true;
    settings.stream.fps = 120;
    settings.client_refresh_rate_x100 = 11994;
    settings_apply_ntsc_preset_refresh(&settings, 120);
    TEST_ASSERT_EQUAL_INT(11988, settings.client_refresh_rate_x100);

    settings.use_ntsc_refresh = false;
    settings_apply_ntsc_preset_refresh(&settings, 120);
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
#endif
}

void testDefaultNtscAndSmoothFlags() {
    TEST_ASSERT_FALSE(settings.use_ntsc_refresh);
#if defined(TARGET_WEBOS)
    TEST_ASSERT_EQUAL_INT(0, settings.client_refresh_rate_x100);
#endif
}

void testIdrRefreshIntervalMsDefaultAndClamp() {
    TEST_ASSERT_EQUAL_INT(0, settings.idr_refresh_interval_ms);

    settings.idr_refresh_interval_ms = 750;
    char *ini_backup = settings.ini_path;
    settings.ini_path = "settings_idr_tmp.ini";
    TEST_ASSERT_TRUE(settings_save(&settings));

    settings.idr_refresh_interval_ms = 0;
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(1000, settings.idr_refresh_interval_ms); /* snapped from 750→1000 via save path */

    /* Re-save exact 500 and confirm round-trip */
    settings.idr_refresh_interval_ms = 500;
    TEST_ASSERT_TRUE(settings_save(&settings));
    settings.idr_refresh_interval_ms = 0;
    TEST_ASSERT_TRUE(settings_read(&settings));
    TEST_ASSERT_EQUAL_INT(500, settings.idr_refresh_interval_ms);
    settings.ini_path = ini_backup;
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(testReadINI);
    RUN_TEST(testWriteINI);
    RUN_TEST(testControllerTouchpadDefaults);
    RUN_TEST(testControllerTouchpadUnknownMode);
    RUN_TEST(testSyncRefreshRate);
    RUN_TEST(testReconcileRefreshRate);
    RUN_TEST(testNtscRefreshRateMapping);
    RUN_TEST(testDefaultNtscAndSmoothFlags);
    RUN_TEST(testIdrRefreshIntervalMsDefaultAndClamp);
    return UNITY_END();
}
