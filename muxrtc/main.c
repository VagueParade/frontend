#include "../lvgl/lvgl.h"
#include "../lvgl/drivers/display/fbdev.h"
#include "../lvgl/drivers/indev/evdev.h"
#include "ui/ui.h"
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <linux/joystick.h>
#include <linux/rtc.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <libgen.h>
#include "../common/common.h"
#include "../common/help.h"
#include "../common/options.h"
#include "../common/theme.h"
#include "../common/config.h"
#include "../common/device.h"
#include "../common/glyph.h"
#include "../common/mini/mini.h"

static int js_fd;

int NAV_DPAD_HOR;
int NAV_ANLG_HOR;
int NAV_DPAD_VER;
int NAV_ANLG_VER;
int NAV_A;
int NAV_B;

int turbo_mode = 0;
int msgbox_active = 0;
int input_disable = 0;
int SD2_found = 0;
int nav_sound = 0;
int safe_quit = 0;
int bar_header = 0;
int bar_footer = 0;
char *osd_message;

struct mux_config config;
struct mux_device device;

int nav_moved = 1;
char *current_wall = "";

lv_obj_t *msgbox_element = NULL;

int progress_onscreen = -1;

int rtcYearValue;
int rtcMonthValue;
int rtcDayValue;
int rtcHourValue;
int rtcMinuteValue;
int rtcTimezoneValue = 0;
int rtcNotationValue = 0;

char rtc_buffer[32];

lv_group_t *ui_group;
lv_group_t *ui_group_value;
lv_group_t *ui_group_glyph;

// Modify the following integer to number of static menu elements
lv_obj_t *ui_objects[7];

const char *notation[] = {
        "12 Hour", "24 Hour"
};

void show_help() {
    char *title = lv_label_get_text(ui_lblTitle);
    char *message = MUXRTC_GENERIC;

    if (strlen(message) <= 1) {
        message = NO_HELP_FOUND;
    }

    show_help_msgbox(ui_pnlHelp, ui_lblHelpHeader, ui_lblHelpContent, title, message);
}

void confirm_rtc_config() {
    int idx_notation = 0;
    char *notation_type = lv_label_get_text(ui_lblNotationValue);

    if (strcmp(notation_type, notation[1]) == 0) {
        idx_notation = 1;
    }

    char config_file[MAX_BUFFER_SIZE];
    snprintf(config_file, sizeof(config_file),
             "%s/config/config.ini", INTERNAL_PATH);

    mini_t * muos_config = mini_try_load(config_file);

    mini_set_int(muos_config, "clock", "notation", idx_notation);

    mini_save(muos_config, MINI_FLAGS_SKIP_EMPTY_GROUPS);
    mini_free(muos_config);

    system("hwclock -w");
}

void restore_clock_settings() {
    FILE * fp;
    char date_output[100];
    int attempts = 0;

    char command[MAX_BUFFER_SIZE];
    snprintf(command, sizeof(command), "date +\"%%Y %%m %%d %%H %%M\"");

    while (attempts < RTC_MAX_RETRIES) {
        fp = popen(command, "r");
        if (fp == NULL) {
            perror("Failed to run date command");
            attempts++;
            sleep(RTC_RETRY_DELAY);
            continue;
        }

        if (fgets(date_output, sizeof(date_output) - 1, fp) != NULL) {
            pclose(fp);
            break;
        } else {
            perror("Failed to read date command output");
            pclose(fp);
            attempts++;
            sleep(RTC_RETRY_DELAY);
            continue;
        }
    }

    if (attempts == RTC_MAX_RETRIES) {
        fprintf(stderr, "Attempts to read system date failed\n");
        return;
    }

    int year, month, day, hour, minute;
    if (sscanf(date_output, "%d %d %d %d %d", &year, &month, &day, &hour, &minute) != 5) {
        fprintf(stderr, "Failed to parse date command output\n");
        return;
    }

    rtcYearValue = year;
    rtcMonthValue = month;
    rtcDayValue = day;
    rtcHourValue = hour;
    rtcMinuteValue = minute;

    snprintf(rtc_buffer, sizeof(rtc_buffer), "%04d", year);
    lv_label_set_text(ui_lblYearValue, rtc_buffer);

    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMonthValue);
    lv_label_set_text(ui_lblMonthValue, rtc_buffer);

    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
    lv_label_set_text(ui_lblDayValue, rtc_buffer);

    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcHourValue);
    lv_label_set_text(ui_lblHourValue, rtc_buffer);

    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMinuteValue);
    lv_label_set_text(ui_lblMinuteValue, rtc_buffer);

    rtcNotationValue = config.CLOCK.NOTATION;
    lv_label_set_text(ui_lblNotationValue, notation[rtcNotationValue]);
}

void save_clock_settings(int year, int month, int day, int hour, int minute) {
    FILE * fp;
    int attempts = 0;

    char command[MAX_BUFFER_SIZE];
    snprintf(command, sizeof(command), "date \"%04d-%02d-%02d %02d:%02d:00\"",
             year, month, day, hour, minute);

    printf("SETTING DATE TO: %s\n", command);

    while (attempts < RTC_MAX_RETRIES) {
        fp = popen(command, "r");
        if (fp == NULL) {
            perror("Failed to run date command");
            attempts++;
            sleep(RTC_RETRY_DELAY);
            continue;
        }

        if (pclose(fp) == -1) {
            perror("Failed to close date command stream");
            attempts++;
            sleep(RTC_RETRY_DELAY);
            continue;
        }

        confirm_rtc_config();

        return;
    }

    fprintf(stderr, "Attempts to set system date failed\n");
}

int days_in_month(int year, int month) {
    int max_days;
    switch (month) {
        case 2:  // February
            if ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0) {
                max_days = 29;
            } else {
                max_days = 28;
            }
            break;
        case 4:  // April
        case 6:  // June
        case 9:  // September
        case 11: // November
            max_days = 30;
            break;
        default:
            max_days = 31;
            break;
    }
    return max_days;
}

void init_navigation_groups() {
    ui_objects[0] = ui_lblYear;
    ui_objects[1] = ui_lblMonth;
    ui_objects[2] = ui_lblDay;
    ui_objects[3] = ui_lblHour;
    ui_objects[4] = ui_lblMinute;
    ui_objects[5] = ui_lblNotation;
    ui_objects[6] = ui_lblTimezone;

    lv_obj_t *ui_objects_value[] = {
            ui_lblYearValue,
            ui_lblMonthValue,
            ui_lblDayValue,
            ui_lblHourValue,
            ui_lblMinuteValue,
            ui_lblNotationValue,
            ui_lblTimezoneValue
    };

    lv_obj_t *ui_objects_icon[] = {
            ui_icoYear,
            ui_icoMonth,
            ui_icoDay,
            ui_icoHour,
            ui_icoMinute,
            ui_icoNotation,
            ui_icoTimezone
    };

    ui_group = lv_group_create();
    ui_group_value = lv_group_create();
    ui_group_glyph = lv_group_create();

    for (unsigned int i = 0; i < sizeof(ui_objects) / sizeof(ui_objects[0]); i++) {
        lv_group_add_obj(ui_group, ui_objects[i]);
    }

    for (unsigned int i = 0; i < sizeof(ui_objects_value) / sizeof(ui_objects_value[0]); i++) {
        lv_group_add_obj(ui_group_value, ui_objects_value[i]);
    }

    for (unsigned int i = 0; i < sizeof(ui_objects_icon) / sizeof(ui_objects_icon[0]); i++) {
        lv_group_add_obj(ui_group_glyph, ui_objects_icon[i]);
    }
}

void *joystick_task() {
    struct input_event ev;
    int epoll_fd;
    struct epoll_event event, events[device.DEVICE.EVENT];

    int JOYHOTKEY_pressed = 0;

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("Error creating EPOLL instance");
        return NULL;
    }

    event.events = EPOLLIN;
    event.data.fd = js_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, js_fd, &event) == -1) {
        perror("Error with EPOLL controller");
        return NULL;
    }

    while (1) {
        int num_events = epoll_wait(epoll_fd, events, device.DEVICE.EVENT, 64);
        if (num_events == -1) {
            perror("Error with EPOLL wait event timer");
            continue;
        }

        for (int i = 0; i < num_events; i++) {
            if (events[i].data.fd == js_fd) {
                int ret = read(js_fd, &ev, sizeof(struct input_event));
                if (ret == -1) {
                    perror("Error reading input");
                    continue;
                }

                struct _lv_obj_t *element_focused = lv_group_get_focused(ui_group);
                switch (ev.type) {
                    case EV_KEY:
                        if (ev.value == 1) {
                            if (msgbox_active) {
                                if (ev.code == NAV_B || ev.code == device.RAW_INPUT.BUTTON.MENU_SHORT) {
                                    play_sound("confirm", nav_sound);
                                    msgbox_active = 0;
                                    progress_onscreen = 0;
                                    lv_obj_add_flag(msgbox_element, LV_OBJ_FLAG_HIDDEN);
                                }
                            } else {
                                if (ev.code == device.RAW_INPUT.BUTTON.MENU_LONG) {
                                    JOYHOTKEY_pressed = 1;
                                } else if (ev.code == NAV_A) {
                                    if (element_focused == ui_lblTimezone) {
                                        play_sound("confirm", nav_sound);
                                        input_disable = 1;
                                        save_clock_settings(rtcYearValue, rtcMonthValue, rtcDayValue,
                                                            rtcHourValue, rtcMinuteValue);
                                        load_mux("timezone");
                                        write_text_to_file(MUOS_PDI_LOAD, "timezone", "w");
                                        safe_quit = 1;
                                    } else {
                                        play_sound("navigate", nav_sound);
                                        if (element_focused == ui_lblYear) {
                                            if (rtcYearValue >= 1970 && rtcYearValue < 2199) {
                                                rtcYearValue++;
                                                rtcDayValue = 1;
                                            }
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%04d", rtcYearValue);
                                            lv_label_set_text(ui_lblYearValue, rtc_buffer);
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                            lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                        } else if (element_focused == ui_lblMonth) {
                                            if (rtcMonthValue < 12) {
                                                rtcMonthValue++;
                                                rtcDayValue = 1;
                                            } else {
                                                rtcMonthValue = 1;
                                                rtcDayValue = 1;
                                            }
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMonthValue);
                                            lv_label_set_text(ui_lblMonthValue, rtc_buffer);
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                            lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                        } else if (element_focused == ui_lblDay) {
                                            if (rtcDayValue < days_in_month(rtcYearValue, rtcMonthValue)) {
                                                rtcDayValue++;
                                            } else {
                                                rtcDayValue = 1;
                                            }
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                            lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                        } else if (element_focused == ui_lblHour) {
                                            if (rtcHourValue < 23) {
                                                rtcHourValue++;
                                            } else {
                                                rtcHourValue = 0;
                                            }
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcHourValue);
                                            lv_label_set_text(ui_lblHourValue, rtc_buffer);
                                        } else if (element_focused == ui_lblMinute) {
                                            if (rtcMinuteValue < 59) {
                                                rtcMinuteValue++;
                                            } else {
                                                rtcMinuteValue = 0;
                                            }
                                            snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMinuteValue);
                                            lv_label_set_text(ui_lblMinuteValue, rtc_buffer);
                                        } else if (element_focused == ui_lblNotation) {
                                            rtcNotationValue++;
                                            if (rtcNotationValue < 0) {
                                                rtcNotationValue = 1;
                                            }
                                            if (rtcNotationValue > 1) {
                                                rtcNotationValue = 0;
                                            }
                                            lv_label_set_text(ui_lblNotationValue, notation[rtcNotationValue]);
                                        }
                                    }
                                } else if (ev.code == NAV_B) {
                                    play_sound("back", nav_sound);
                                    input_disable = 1;

                                    osd_message = "Saving Changes";
                                    lv_label_set_text(ui_lblMessage, osd_message);
                                    lv_obj_clear_flag(ui_pnlMessage, LV_OBJ_FLAG_HIDDEN);
                                    save_clock_settings(rtcYearValue, rtcMonthValue, rtcDayValue,
                                                        rtcHourValue, rtcMinuteValue);

                                    lv_task_handler();
                                    usleep(device.SCREEN.WAIT);

                                    char config_file[MAX_BUFFER_SIZE];
                                    snprintf(config_file, sizeof(config_file),
                                             "%s/config/config.ini", INTERNAL_PATH);

                                    mini_t * muos_config = mini_try_load(config_file);
                                    mini_set_int(muos_config, "boot", "clock_setup", 0);
                                    mini_save(muos_config, MINI_FLAGS_SKIP_EMPTY_GROUPS);
                                    mini_free(muos_config);

                                    write_text_to_file(MUOS_PDI_LOAD, "clock", "w");
                                    safe_quit = 1;
                                }
                            }
                        } else {
                            if (ev.code == device.RAW_INPUT.BUTTON.MENU_SHORT ||
                                ev.code == device.RAW_INPUT.BUTTON.MENU_LONG) {
                                JOYHOTKEY_pressed = 0;
                                /* DISABLED HELP SCREEN TEMPORARILY
                                if (progress_onscreen == -1) {
                                    play_sound("confirm", nav_sound);
                                    show_help();
                                }
                                */
                            }
                        }
                    case EV_ABS:
                        if (msgbox_active) {
                            break;
                        }
                        if (ev.code == NAV_DPAD_VER || ev.code == NAV_ANLG_VER) {
                            if ((ev.value >= ((device.INPUT.AXIS_MAX >> 2) * -1) &&
                                 ev.value <= ((device.INPUT.AXIS_MIN >> 2) * -1)) ||
                                ev.value == -1) {
                                nav_prev(ui_group, 1);
                                nav_prev(ui_group_value, 1);
                                nav_prev(ui_group_glyph, 1);
                                play_sound("navigate", nav_sound);
                                nav_moved = 1;
                            } else if ((ev.value >= (device.INPUT.AXIS_MIN >> 2) &&
                                        ev.value <= (device.INPUT.AXIS_MAX >> 2)) ||
                                       ev.value == 1) {
                                nav_next(ui_group, 1);
                                nav_next(ui_group_value, 1);
                                nav_next(ui_group_glyph, 1);
                                play_sound("navigate", nav_sound);
                                nav_moved = 1;
                            }
                        } else if (ev.code == NAV_DPAD_HOR || ev.code == NAV_ANLG_HOR) {
                            if ((ev.value >= ((device.INPUT.AXIS_MAX >> 2) * -1) &&
                                 ev.value <= ((device.INPUT.AXIS_MIN >> 2) * -1)) ||
                                ev.value == -1) {
                                play_sound("navigate", nav_sound);
                                if (element_focused == ui_lblYear) {
                                    if (rtcYearValue > 1970 && rtcYearValue <= 2199) {
                                        rtcYearValue--;
                                        rtcDayValue = 1;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%04d", rtcYearValue);
                                    lv_label_set_text(ui_lblYearValue, rtc_buffer);
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblMonth) {
                                    if (rtcMonthValue > 1) {
                                        rtcMonthValue--;
                                        rtcDayValue = 1;
                                    } else {
                                        rtcMonthValue = 12;
                                        rtcDayValue = 1;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMonthValue);
                                    lv_label_set_text(ui_lblMonthValue, rtc_buffer);
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblDay) {
                                    if (rtcDayValue > 1) {
                                        rtcDayValue--;
                                    } else {
                                        rtcDayValue = days_in_month(rtcYearValue, rtcMonthValue);
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblHour) {
                                    if (rtcHourValue > 0) {
                                        rtcHourValue--;
                                    } else {
                                        rtcHourValue = 23;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcHourValue);
                                    lv_label_set_text(ui_lblHourValue, rtc_buffer);
                                } else if (element_focused == ui_lblMinute) {
                                    if (rtcMinuteValue > 0) {
                                        rtcMinuteValue--;
                                    } else {
                                        rtcMinuteValue = 59;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMinuteValue);
                                    lv_label_set_text(ui_lblMinuteValue, rtc_buffer);
                                } else if (element_focused == ui_lblNotation) {
                                    rtcNotationValue--;
                                    if (rtcNotationValue < 0) {
                                        rtcNotationValue = 1;
                                    }
                                    if (rtcNotationValue > 1) {
                                        rtcNotationValue = 0;
                                    }

                                    lv_label_set_text(ui_lblNotationValue, notation[rtcNotationValue]);
                                }
                            } else if ((ev.value >= (device.INPUT.AXIS_MIN >> 2) &&
                                        ev.value <= (device.INPUT.AXIS_MAX >> 2)) ||
                                       ev.value == 1) {
                                play_sound("navigate", nav_sound);
                                if (element_focused == ui_lblYear) {
                                    if (rtcYearValue >= 1970 && rtcYearValue < 2199) {
                                        rtcYearValue++;
                                        rtcDayValue = 1;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%04d", rtcYearValue);
                                    lv_label_set_text(ui_lblYearValue, rtc_buffer);
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblMonth) {
                                    if (rtcMonthValue < 12) {
                                        rtcMonthValue++;
                                        rtcDayValue = 1;
                                    } else {
                                        rtcMonthValue = 1;
                                        rtcDayValue = 1;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMonthValue);
                                    lv_label_set_text(ui_lblMonthValue, rtc_buffer);
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblDay) {
                                    if (rtcDayValue < days_in_month(rtcYearValue, rtcMonthValue)) {
                                        rtcDayValue++;
                                    } else {
                                        rtcDayValue = 1;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcDayValue);
                                    lv_label_set_text(ui_lblDayValue, rtc_buffer);
                                } else if (element_focused == ui_lblHour) {
                                    if (rtcHourValue < 23) {
                                        rtcHourValue++;
                                    } else {
                                        rtcHourValue = 0;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcHourValue);
                                    lv_label_set_text(ui_lblHourValue, rtc_buffer);
                                } else if (element_focused == ui_lblMinute) {
                                    if (rtcMinuteValue < 59) {
                                        rtcMinuteValue++;
                                    } else {
                                        rtcMinuteValue = 0;
                                    }
                                    snprintf(rtc_buffer, sizeof(rtc_buffer), "%02d", rtcMinuteValue);
                                    lv_label_set_text(ui_lblMinuteValue, rtc_buffer);
                                } else if (element_focused == ui_lblNotation) {
                                    rtcNotationValue++;
                                    if (rtcNotationValue < 0) {
                                        rtcNotationValue = 1;
                                    }
                                    if (rtcNotationValue > 1) {
                                        rtcNotationValue = 0;
                                    }
                                    lv_label_set_text(ui_lblNotationValue, notation[rtcNotationValue]);
                                }
                            }
                        }
                    default:
                        break;
                }
            }
        }

        if (ev.type == EV_KEY && ev.value == 1 &&
            (ev.code == device.RAW_INPUT.BUTTON.VOLUME_DOWN || ev.code == device.RAW_INPUT.BUTTON.VOLUME_UP)) {
            if (JOYHOTKEY_pressed) {
                progress_onscreen = 1;
                lv_obj_add_flag(ui_pnlProgressVolume, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_pnlProgressBrightness, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(ui_icoProgressBrightness, "\uF185");
                lv_bar_set_value(ui_barProgressBrightness, atoi(read_text_from_file(BRIGHT_PERC)), LV_ANIM_OFF);
            } else {
                progress_onscreen = 2;
                lv_obj_add_flag(ui_pnlProgressBrightness, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_pnlProgressVolume, LV_OBJ_FLAG_HIDDEN);
                int volume = atoi(read_text_from_file(VOLUME_PERC));
                switch (volume) {
                    case 0:
                        lv_label_set_text(ui_icoProgressVolume, "\uF6A9");
                        break;
                    case 1 ... 46:
                        lv_label_set_text(ui_icoProgressVolume, "\uF026");
                        break;
                    case 47 ... 71:
                        lv_label_set_text(ui_icoProgressVolume, "\uF027");
                        break;
                    case 72 ... 100:
                        lv_label_set_text(ui_icoProgressVolume, "\uF028");
                        break;
                }
                lv_bar_set_value(ui_barProgressVolume, volume, LV_ANIM_OFF);
            }
        }

        lv_task_handler();
        usleep(device.SCREEN.WAIT);
    }
}

void init_elements() {
    lv_obj_move_foreground(ui_pnlFooter);
    lv_obj_move_foreground(ui_pnlHeader);
    lv_obj_move_foreground(ui_pnlHelp);
    lv_obj_move_foreground(ui_pnlProgressBrightness);
    lv_obj_move_foreground(ui_pnlProgressVolume);

    if (bar_footer) {
        lv_obj_set_style_bg_opa(ui_pnlFooter, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (bar_header) {
        lv_obj_set_style_bg_opa(ui_pnlHeader, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    process_visual_element(CLOCK, ui_lblDatetime);
    process_visual_element(BLUETOOTH, ui_staBluetooth);
    process_visual_element(NETWORK, ui_staNetwork);
    process_visual_element(BATTERY, ui_staCapacity);

    lv_label_set_text(ui_lblMessage, osd_message);

    lv_label_set_text(ui_lblNavB, "Save");

    lv_obj_t *nav_hide[] = {
            ui_lblNavAGlyph,
            ui_lblNavA,
            ui_lblNavCGlyph,
            ui_lblNavC,
            ui_lblNavXGlyph,
            ui_lblNavX,
            ui_lblNavYGlyph,
            ui_lblNavY,
            ui_lblNavZGlyph,
            ui_lblNavZ,
            ui_lblNavMenuGlyph,
            ui_lblNavMenu
    };

    for (int i = 0; i < sizeof(nav_hide) / sizeof(nav_hide[0]); i++) {
        lv_obj_add_flag(nav_hide[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(nav_hide[i], LV_OBJ_FLAG_FLOATING);
    }

    lv_obj_set_user_data(ui_lblYear, "year");
    lv_obj_set_user_data(ui_lblMonth, "month");
    lv_obj_set_user_data(ui_lblDay, "day");
    lv_obj_set_user_data(ui_lblHour, "hour");
    lv_obj_set_user_data(ui_lblMinute, "minute");
    lv_obj_set_user_data(ui_lblNotation, "notation");
    lv_obj_set_user_data(ui_lblTimezone, "timezone");

    char *overlay = load_overlay_image();
    if (strlen(overlay) > 0 && theme.MISC.IMAGE_OVERLAY) {
        lv_obj_t * overlay_img = lv_img_create(ui_scrRTC);
        lv_img_set_src(overlay_img, overlay);
        lv_obj_move_foreground(overlay_img);
    }

    if (TEST_IMAGE) display_testing_message(ui_scrRTC);
}

void glyph_task() {
    // TODO: Bluetooth connectivity!

    if (device.DEVICE.HAS_NETWORK && is_network_connected()) {
        lv_obj_set_style_text_color(ui_staNetwork, lv_color_hex(theme.STATUS.NETWORK.ACTIVE),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ui_staNetwork, theme.STATUS.NETWORK.ACTIVE_ALPHA, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_text_color(ui_staNetwork, lv_color_hex(theme.STATUS.NETWORK.NORMAL),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ui_staNetwork, theme.STATUS.NETWORK.NORMAL_ALPHA, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (atoi(read_text_from_file(device.BATTERY.CHARGER))) {
        lv_obj_set_style_text_color(ui_staCapacity, lv_color_hex(theme.STATUS.BATTERY.ACTIVE),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ui_staCapacity, theme.STATUS.BATTERY.ACTIVE_ALPHA, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else if (read_battery_capacity() <= 15) {
        lv_obj_set_style_text_color(ui_staCapacity, lv_color_hex(theme.STATUS.BATTERY.LOW),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ui_staCapacity, theme.STATUS.BATTERY.LOW_ALPHA, LV_PART_MAIN | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_text_color(ui_staCapacity, lv_color_hex(theme.STATUS.BATTERY.NORMAL),
                                    LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_opa(ui_staCapacity, theme.STATUS.BATTERY.NORMAL_ALPHA, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    if (progress_onscreen > 0) {
        progress_onscreen -= 1;
    } else {
        if (!lv_obj_has_flag(ui_pnlProgressBrightness, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(ui_pnlProgressBrightness, LV_OBJ_FLAG_HIDDEN);
        }
        if (!lv_obj_has_flag(ui_pnlProgressVolume, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(ui_pnlProgressVolume, LV_OBJ_FLAG_HIDDEN);
        }
        if (!msgbox_active) {
            progress_onscreen = -1;
        }
    }
}

void ui_refresh_task() {
    lv_bar_set_value(ui_barProgressBrightness, atoi(read_text_from_file(BRIGHT_PERC)), LV_ANIM_OFF);
    lv_bar_set_value(ui_barProgressVolume, atoi(read_text_from_file(VOLUME_PERC)), LV_ANIM_OFF);

    if (nav_moved) {
        if (lv_group_get_obj_count(ui_group) > 0) {
            if (config.BOOT.FACTORY_RESET) {
                char init_wall[MAX_BUFFER_SIZE];
                snprintf(init_wall, sizeof(init_wall), "M:%s/theme/image/wall/default.png", INTERNAL_PATH);
                lv_img_set_src(ui_imgWall, init_wall);
            } else {
                static char old_wall[MAX_BUFFER_SIZE];
                static char new_wall[MAX_BUFFER_SIZE];

                snprintf(old_wall, sizeof(old_wall), "%s", current_wall);
                snprintf(new_wall, sizeof(new_wall), "%s", load_wallpaper(
                        ui_scrRTC, ui_group, theme.MISC.ANIMATED_BACKGROUND));

                if (strcasecmp(new_wall, old_wall) != 0) {
                    strcpy(current_wall, new_wall);
                    if (strlen(new_wall) > 3) {
                        printf("LOADING WALLPAPER: %s\n", new_wall);
                        if (theme.MISC.ANIMATED_BACKGROUND) {
                            lv_obj_t * img = lv_gif_create(ui_pnlWall);
                            lv_gif_set_src(img, new_wall);
                        } else {
                            lv_img_set_src(ui_imgWall, new_wall);
                        }
                        lv_obj_invalidate(ui_pnlWall);
                    } else {
                        lv_img_set_src(ui_imgWall, &ui_img_nothing_png);
                    }
                }
            }

            static char static_image[MAX_BUFFER_SIZE];
            snprintf(static_image, sizeof(static_image), "%s",
                     load_static_image(ui_scrRTC, ui_group));

            if (strlen(static_image) > 0) {
                printf("LOADING STATIC IMAGE: %s\n", static_image);

                switch (theme.MISC.STATIC_ALIGNMENT) {
                    case 0: // Bottom + Front
                        lv_obj_set_align(ui_imgBox, LV_ALIGN_BOTTOM_RIGHT);
                        lv_obj_move_foreground(ui_pnlBox);
                        break;
                    case 1: // Middle + Front
                        lv_obj_set_align(ui_imgBox, LV_ALIGN_RIGHT_MID);
                        lv_obj_move_foreground(ui_pnlBox);
                        break;
                    case 2: // Top + Front
                        lv_obj_set_align(ui_imgBox, LV_ALIGN_TOP_RIGHT);
                        lv_obj_move_foreground(ui_pnlBox);
                        break;
                    case 3: // Fullscreen + Behind
                        lv_obj_set_height(ui_pnlBox, device.MUX.HEIGHT);
                        lv_obj_set_align(ui_imgBox, LV_ALIGN_BOTTOM_RIGHT);
                        lv_obj_move_background(ui_pnlBox);
                        lv_obj_move_background(ui_pnlWall);
                        break;
                    case 4: // Fullscreen + Front
                        lv_obj_set_height(ui_pnlBox, device.MUX.HEIGHT);
                        lv_obj_set_align(ui_imgBox, LV_ALIGN_BOTTOM_RIGHT);
                        lv_obj_move_foreground(ui_pnlBox);
                        break;
                }

                lv_img_set_src(ui_imgBox, static_image);
            } else {
                lv_img_set_src(ui_imgBox, &ui_img_nothing_png);
            }
        }
        lv_obj_invalidate(ui_pnlContent);
        lv_task_handler();
        nav_moved = 0;
    }
}

void direct_to_previous() {
    if (file_exist(MUOS_PDI_LOAD)) {
        char *prev = read_text_from_file(MUOS_PDI_LOAD);
        int text_hit = 0;

        for (unsigned int i = 0; i < sizeof(ui_objects) / sizeof(ui_objects[0]); i++) {
            const char *u_data = lv_obj_get_user_data(ui_objects[i]);

            if (strcasecmp(u_data, prev) == 0) {
                text_hit = i;
                break;
            }
        }

        if (text_hit != 0) {
            nav_next(ui_group, text_hit);
            nav_next(ui_group_glyph, text_hit);
            nav_next(ui_group_value, text_hit);
            nav_moved = 1;
        }
    }
}

int main(int argc, char *argv[]) {
    load_device(&device);

    srand(time(NULL));

    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin:/system/bin", 1);
    setenv("NO_COLOR", "1", 1);

    lv_init();
    fbdev_init(device.SCREEN.DEVICE);

    static lv_disp_draw_buf_t disp_buf;
    uint32_t disp_buf_size = device.SCREEN.BUFFER;

    lv_color_t * buf1 = (lv_color_t *) malloc(disp_buf_size * sizeof(lv_color_t));
    lv_color_t * buf2 = (lv_color_t *) malloc(disp_buf_size * sizeof(lv_color_t));

    lv_disp_draw_buf_init(&disp_buf, buf1, buf2, disp_buf_size);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &disp_buf;
    disp_drv.flush_cb = fbdev_flush;
    disp_drv.hor_res = device.SCREEN.WIDTH;
    disp_drv.ver_res = device.SCREEN.HEIGHT;
    disp_drv.sw_rotate = device.SCREEN.ROTATE;
    disp_drv.rotated = device.SCREEN.ROTATE;
    lv_disp_drv_register(&disp_drv);

    load_config(&config);

    ui_init();
    init_elements();

    lv_obj_set_user_data(ui_scrRTC, basename(argv[0]));

    lv_label_set_text(ui_lblDatetime, get_datetime());
    lv_label_set_text(ui_staCapacity, get_capacity());

    load_theme(&theme, &config, &device, basename(argv[0]));
    apply_theme();

    switch (theme.MISC.NAVIGATION_TYPE) {
        case 1:
            NAV_DPAD_HOR = device.RAW_INPUT.DPAD.DOWN;
            NAV_ANLG_HOR = device.RAW_INPUT.ANALOG.LEFT.DOWN;
            NAV_DPAD_VER = device.RAW_INPUT.DPAD.RIGHT;
            NAV_ANLG_VER = device.RAW_INPUT.ANALOG.LEFT.RIGHT;
            break;
        default:
            NAV_DPAD_HOR = device.RAW_INPUT.DPAD.RIGHT;
            NAV_ANLG_HOR = device.RAW_INPUT.ANALOG.LEFT.RIGHT;
            NAV_DPAD_VER = device.RAW_INPUT.DPAD.DOWN;
            NAV_ANLG_VER = device.RAW_INPUT.ANALOG.LEFT.DOWN;
    }

    switch (config.SETTINGS.ADVANCED.SWAP) {
        case 1:
            NAV_A = device.RAW_INPUT.BUTTON.B;
            NAV_B = device.RAW_INPUT.BUTTON.A;
            lv_label_set_text(ui_lblNavAGlyph, "\u21D2");
            lv_label_set_text(ui_lblNavBGlyph, "\u21D3");
            break;
        default:
            NAV_A = device.RAW_INPUT.BUTTON.A;
            NAV_B = device.RAW_INPUT.BUTTON.B;
            lv_label_set_text(ui_lblNavAGlyph, "\u21D3");
            lv_label_set_text(ui_lblNavBGlyph, "\u21D2");
            break;
    }

    current_wall = load_wallpaper(ui_scrRTC, NULL, theme.MISC.ANIMATED_BACKGROUND);
    if (strlen(current_wall) > 3) {
        if (theme.MISC.ANIMATED_BACKGROUND) {
            lv_obj_t * img = lv_gif_create(ui_pnlWall);
            lv_gif_set_src(img, current_wall);
        } else {
            lv_img_set_src(ui_imgWall, current_wall);
        }
    } else {
        lv_img_set_src(ui_imgWall, &ui_img_nothing_png);
    }

    load_font_text(basename(argv[0]), ui_scrRTC);

    if (config.SETTINGS.GENERAL.SOUND == 2) {
        nav_sound = 1;
    }

    init_navigation_groups();
    restore_clock_settings();

    struct dt_task_param dt_par;
    struct bat_task_param bat_par;
    struct osd_task_param osd_par;

    dt_par.lblDatetime = ui_lblDatetime;
    bat_par.staCapacity = ui_staCapacity;
    osd_par.lblMessage = ui_lblMessage;
    osd_par.pnlMessage = ui_pnlMessage;
    osd_par.count = 0;

    js_fd = open(device.INPUT.EV1, O_RDONLY);
    if (js_fd < 0) {
        perror("Failed to open joystick device");
        return 1;
    }

    lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);

    indev_drv.type = LV_INDEV_TYPE_KEYPAD;
    indev_drv.read_cb = evdev_read;
    indev_drv.user_data = (void *) (intptr_t) js_fd;

    lv_indev_drv_register(&indev_drv);

    lv_timer_t *datetime_timer = lv_timer_create(datetime_task, UINT16_MAX / 2, &dt_par);
    lv_timer_ready(datetime_timer);

    lv_timer_t *capacity_timer = lv_timer_create(capacity_task, UINT16_MAX / 2, &bat_par);
    lv_timer_ready(capacity_timer);

    lv_timer_t *osd_timer = lv_timer_create(osd_task, UINT16_MAX / 32, &osd_par);
    lv_timer_ready(osd_timer);

    lv_timer_t *glyph_timer = lv_timer_create(glyph_task, UINT16_MAX / 64, NULL);
    lv_timer_ready(glyph_timer);

    lv_timer_t *ui_refresh_timer = lv_timer_create(ui_refresh_task, UINT8_MAX / 4, NULL);
    lv_timer_ready(ui_refresh_timer);

    pthread_t joystick_thread;
    pthread_create(&joystick_thread, NULL, (void *(*)(void *)) joystick_task, NULL);

    init_elements();
    direct_to_previous();

    while (!safe_quit) {
        usleep(device.SCREEN.WAIT);
    }

    pthread_cancel(joystick_thread);

    close(js_fd);

    return 0;
}

uint32_t mux_tick(void) {
    static uint64_t start_ms = 0;

    if (start_ms == 0) {
        struct timeval tv_start;
        gettimeofday(&tv_start, NULL);
        start_ms = (tv_start.tv_sec * 1000000 + tv_start.tv_usec) / 1000;
    }

    struct timeval tv_now;
    gettimeofday(&tv_now, NULL);

    uint64_t now_ms;
    now_ms = (tv_now.tv_sec * 1000000 + tv_now.tv_usec) / 1000;

    uint32_t time_ms = now_ms - start_ms;
    return time_ms;
}
