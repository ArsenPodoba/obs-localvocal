#include "transcription-utils.h"

#include <sstream>
#include <algorithm>
#include <vector>
#include <fstream>
#include <filesystem>

// clang-format off
#define is_lead_byte(c) (((c)&0xe0) == 0xc0 || ((c)&0xf0) == 0xe0 || ((c)&0xf8) == 0xf0)
// clang-format off
#define is_trail_byte(c) (((c)&0xc0) == 0x80)

inline int lead_byte_length(const uint8_t c)
{
	if ((c & 0xe0) == 0xc0) {
		return 2;
	} else if ((c & 0xf0) == 0xe0) {
		return 3;
	} else if ((c & 0xf8) == 0xf0) {
		return 4;
	} else {
		return 1;
	}
}

inline bool is_valid_lead_byte(const uint8_t *c)
{
	const int length = lead_byte_length(c[0]);
	if (length == 1) {
		return true;
	}
	if (length == 2 && is_trail_byte(c[1])) {
		return true;
	}
	if (length == 3 && is_trail_byte(c[1]) && is_trail_byte(c[2])) {
		return true;
	}
	if (length == 4 && is_trail_byte(c[1]) && is_trail_byte(c[2]) && is_trail_byte(c[3])) {
		return true;
	}
	return false;
}

std::string fix_utf8(const std::string &str)
{
#ifdef _WIN32
	// Some UTF8 charsets on Windows output have a bug, instead of 0xd? it outputs
	// 0xf?, and 0xc? becomes 0xe?, so we need to fix it.
	std::stringstream ss;
	uint8_t *c_str = (uint8_t *)str.c_str();
	for (size_t i = 0; i < str.size(); ++i) {
		if (is_lead_byte(c_str[i])) {
			// this is a unicode leading byte
			// if the next char is 0xff - it's a bug char, replace it with 0x9f
			if (c_str[i + 1] == 0xff) {
				c_str[i + 1] = 0x9f;
			}
			if (!is_valid_lead_byte(c_str + i)) {
				// This is a bug lead byte, because it's length 3 and the i+2 byte is also
				// a lead byte
				c_str[i] = c_str[i] - 0x20;
			}
		} else {
			if (c_str[i] >= 0xf8) {
				// this may be a malformed lead byte.
				// lets see if it becomes a valid lead byte if we "fix" it
				uint8_t buf_[4];
				buf_[0] = c_str[i] - 0x20;
				buf_[1] = c_str[i + 1];
				buf_[2] = c_str[i + 2];
				buf_[3] = c_str[i + 3];
				if (is_valid_lead_byte(buf_)) {
					// this is a malformed lead byte, fix it
					c_str[i] = c_str[i] - 0x20;
				}
			}
		}
	}

	return std::string((char *)c_str);
#else
	return str;
#endif
}

/*
* Remove leading and trailing non-alphabetic characters from a string.
* This function is used to remove leading and trailing spaces, newlines, tabs or punctuation.
* @param str: the string to remove leading and trailing non-alphabetic characters from.
* @return: the string with leading and trailing non-alphabetic characters removed.
*/
std::string remove_leading_trailing_nonalpha(const std::string &str)
{
	if (str.size() == 0) {
		return str;
	}
	if (str.size() == 1) {
		if (std::isalpha(str[0])) {
			return str;
		} else {
			return "";
		}
	}
	if (str.size() == 2) {
		if (std::isalpha(str[0]) && std::isalpha(str[1])) {
			return str;
		} else if (std::isalpha(str[0])) {
			return std::string(1, str[0]);
		} else if (std::isalpha(str[1])) {
			return std::string(1, str[1]);
		} else {
			return "";
		}
	}
	std::string str_copy = str;
	// remove trailing spaces, newlines, tabs or punctuation
	auto last_non_space =
		std::find_if(str_copy.rbegin(), str_copy.rend(), [](unsigned char ch) {
			return !std::isspace(ch) || !std::ispunct(ch);
		}).base();
	str_copy.erase(last_non_space, str_copy.end());
	// remove leading spaces, newlines, tabs or punctuation
	auto first_non_space = std::find_if(str_copy.begin(), str_copy.end(),
					    [](unsigned char ch) {
						    return !std::isspace(ch) || !std::ispunct(ch);
					    }) +
			       1;
	str_copy.erase(str_copy.begin(), first_non_space);
	return str_copy;
}

std::vector<std::string> split(const std::string &string, char delimiter)
{
	std::vector<std::string> tokens;
	std::string token;
	std::istringstream tokenStream(string);
	while (std::getline(tokenStream, token, delimiter)) {
		if (!token.empty()) {
			tokens.push_back(token);
		}
	}
	return tokens;
}

std::vector<std::string> split_words(const std::string &str_copy)
{
	std::vector<std::string> words;
	std::string word;
	for (char c : str_copy) {
		if (std::isspace(c)) {
			if (!word.empty()) {
				words.push_back(word);
				word.clear();
			}
		} else {
			word += c;
		}
	}
	if (!word.empty()) {
		words.push_back(word);
	}
	return words;
}

void clear_output_files_on_start(const std::filesystem::path &output_file_path, const std::map<std::string, std::string> &language_codes_to_whisper)
{
	namespace fs = std::filesystem;

	if (output_file_path.string().empty()) {
		return;
	}

	const auto base_file_name = output_file_path.stem().string();
	const auto fie_extension  = output_file_path.extension().string();
	const auto file_parent_path = output_file_path.parent_path();

	// main file
	{
		const auto output_file = std::ofstream(output_file_path.string(), std::ios::out | std::ios::trunc);
	}

	// translations — only if the file exists
	for (const auto& [language_code, _] : language_codes_to_whisper) {
		const auto target_language_output_file_path = file_parent_path / (base_file_name + "_" + language_code + fie_extension);
		if (fs::exists(target_language_output_file_path)) {
			const auto target_language_output_file = std::ofstream(target_language_output_file_path.string(), std::ios::out | std::ios::trunc);
		}
	}
}

size_t get_last_line_length(const std::string& file_path)
{
    auto file = std::ifstream(file_path, std::ios::ate);
    if (!file.is_open())
        return 0;

    const auto file_size = file.tellg();
    if (file_size <= 0)
        return 0;

    auto ch = char();
    auto pos = std::streamoff(1);

    for (; pos <= file_size; ++pos) {
        file.seekg(-pos, std::ios::end);
        file.get(ch);
        if (ch == '\n')
            break;
    }

    return static_cast<size_t>(pos - 1);
}

std::vector<std::string> split_into_lines(const std::string& text, size_t max_len, size_t current_line_size)
{
	std::vector<std::string> lines;
	if (max_len == 0 || text.empty()) {
		return lines;
	}

	auto iss = std::istringstream(text);
	auto word = std::string();
	auto segment = std::string();

	while (iss >> word) {
		const auto add_len = word.size() + (current_line_size > 0 ? 1 : 0);

		if (current_line_size + add_len <= max_len) {
			segment += (current_line_size > 0 ? " " : "") + word;
			current_line_size += add_len;
		} else {
			lines.emplace_back(segment + "\n");
			segment = word;
			current_line_size = word.size();
		}
	}

	if (!segment.empty()) {
		lines.emplace_back(segment);
	}

	return lines;
}
