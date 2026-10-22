#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化黑客帝国主题 GUI、雨滴特效及时间显示
 */
void init_passport_main_gui(void);

/**
 * @brief 初始化主界面 UI（时间、日期、星期）
 */
void gui_main_init(void);

/**
 * @brief 动态更新时间、日期和星期
 */
void gui_update_time_date(const char * time_str, const char * date_str, const char * week_str);

#ifdef __cplusplus
}
#endif