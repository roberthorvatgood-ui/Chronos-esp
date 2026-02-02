
#pragma once
enum Language {
    LANG_EN = 0,
    LANG_HR,
    LANG_DE,
    LANG_FR,
    LANG_ES
};

void i18n_init();
Language i18n_get_language();
const char* i18n_get_lang_code();
void i18n_set_language(Language lang);
void i18n_set_lang_code(const char* code);
const char* tr(const char* key);
void i18n_load_saved();
