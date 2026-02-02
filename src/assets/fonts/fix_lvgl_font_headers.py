
import os
import re

def fix_lvgl_header_in_file(filepath):
    with open(filepath, 'r', encoding='utf-8') as f:
        content = f.read()

    # Pattern matches both "lvgl.h" and <lvgl.h> variants
    pattern = re.compile(
        r'#ifdef LV_LVGL_H_INCLUDE_SIMPLE\s*#include [<"]lvgl\.h[>"]\s*#else\s*#include [<"]lvgl(?:/lvgl)?\.h[>"]\s*#endif',
        re.MULTILINE
    )

    new_content, count = pattern.subn('#include <lvgl.h>', content)
    if count > 0:
        with open(filepath, 'w', encoding='utf-8') as f:
            f.write(new_content)
        print(f"Fixed header in: {filepath}")
    else:
        print(f"No change needed: {filepath}")

def fix_all_font_headers(directory):
    for filename in os.listdir(directory):
        if filename.endswith('.c'):
            fix_lvgl_header_in_file(os.path.join(directory, filename))

if __name__ == "__main__":
    # Change this to your fonts directory if needed
    fonts_dir = os.path.dirname(os.path.abspath(__file__))
    fix_all_font_headers(fonts_dir)
