#pragma once

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace ss2 {

// Languages of the native build's own interface text. Mission text comes from the
// game data and is not translated here.
enum class Language { english, french, spanish };

void setLanguage(Language);
Language language();

// "en", "fr", "es", also with a region or encoding ("fr_FR.UTF-8", "es-MX"), any case.
std::optional<Language> languageFromCode(std::string_view code);
// From LC_ALL, LC_MESSAGES or LANG, in that order; English when none names a known language.
Language languageFromEnvironment();

// The current language's text for an English interface string. English is the key:
// a string with no entry comes back unchanged, so untranslated text is still shown.
std::string tr(std::string_view english);
// The same, then {0}, {1}, ... replaced by the arguments. Put whole sentences in the
// key so each language can order its words: trf("Move order accepted by {0} of {1} soldiers", {a, b}).
std::string trf(std::string_view english, std::initializer_list<std::string> arguments);

// Every English key that has translations, for tests and for checking coverage.
std::size_t translationCount();
std::string_view translationKey(std::size_t index);
}
