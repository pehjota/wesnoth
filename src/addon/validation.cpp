/*
   Copyright (C) 2003 - 2008 by David White <dave@whitevine.net>
                 2008 - 2015 by Iris Morelle <shadowm2006@gmail.com>
   Part of the Battle for Wesnoth Project https://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

#include "addon/validation.hpp"
#include "config.hpp"
#include "serialization/unicode_cast.hpp"
#include "hash.hpp"

#include <algorithm>
#include <array>
#include <boost/algorithm/string.hpp>
#include <set>

const unsigned short default_campaignd_port = 15015;

namespace
{

const std::array<std::string, ADDON_TYPES_COUNT> addon_type_strings {{
	"unknown", "core", "campaign", "scenario", "campaign_sp_mp", "campaign_mp",
	"scenario_mp", "map_pack", "era", "faction", "mod_mp", /*"gui", */ "media",
	"other"
}};

// Reserved DOS device names on Windows XP and later.
const std::set<std::string> dos_device_names = {
	"NUL", "CON", "AUX", "PRN",
	// Console API devices
	"CONIN$", "CONOUT$",
	// Configuration-dependent devices
	"COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
	"LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
};

struct addon_name_char_illegal
{
	/**
	 * Returns whether the given add-on name char is not whitelisted.
	 */
	inline bool operator()(char c) const
	{
		switch(c) {
			case '-':		// hyphen-minus
			case '_':		// low line
				return false;
			default:
				return !isalnum(c);
		}
	}
};

struct addon_filename_ucs4char_illegal
{
	inline bool operator()(char32_t c) const
	{
		switch(c) {
			case ' ':
			case '"':
			case '*':
			case '/':
			case ':':
			case '<':
			case '>':
			case '?':
			case '\\':
			case '|':
			case '~':
			case 0x7F: // DEL
				return true;
			default:
				return (
					c < 0x20 ||                 // C0 control characters
					(c >= 0x80 && c < 0xA0) ||  // C1 control characters
					(c >= 0xD800 && c < 0xE000) // surrogate pairs
				);
		}
	}
};

} // end unnamed namespace

bool addon_name_legal(const std::string& name)
{
	if(name.empty() ||
	   std::find_if(name.begin(), name.end(), addon_name_char_illegal()) != name.end()) {
		return false;
	} else {
	   return true;
	}
}

bool addon_filename_legal(const std::string& name)
{
	if(name.empty() || name.back() == '.' ||
	   name.find("..") != std::string::npos ||
	   name.size() > 255) {
		return false;
	} else {
		// NOTE: We can't use filesystem::base_name() here, since it returns
		//       the filename up to the *last* dot. "CON.foo.bar" in
		//       "CON.foo.bar.baz" is still redirected to "CON" on Windows;
		//       the base_name() approach would cause the name to not match
		//       any entries on our blacklist.
		//       Do also note that we're relying on the next check after this
		//       to flag the name as illegal if it contains a ':' -- a
		//       trailing colon is a valid way to refer to DOS device names,
		//       meaning that e.g. "CON:" is equivalent to "CON".
		const std::string stem = boost::algorithm::to_upper_copy(name.substr(0, name.find('.')), std::locale::classic());
		if(dos_device_names.find(stem) != dos_device_names.end()) {
			return false;
		}

		const std::u32string name_ucs4 = unicode_cast<std::u32string>(name);
		const std::string name_utf8 = unicode_cast<std::string>(name_ucs4);
		if(name != name_utf8){ // name is invalid UTF-8
			return false;
		}
		return std::find_if(name_ucs4.begin(), name_ucs4.end(), addon_filename_ucs4char_illegal()) == name_ucs4.end();
	}
}

namespace {

bool check_names_legal_internal(const config& dir, std::string current_prefix, std::vector<std::string>* badlist)
{
	if (!current_prefix.empty()) {
		current_prefix += '/';
	}

	for(const config& path : dir.child_range("file")) {
		const std::string& filename = path["name"];

		if(!addon_filename_legal(filename)) {
			if(badlist) {
				badlist->push_back(current_prefix + filename);
			} else {
				return false;
			}
		}
	}

	for(const config& path : dir.child_range("dir")) {
		const std::string& dirname = path["name"];
		const std::string& new_prefix = current_prefix + dirname;

		if(!addon_filename_legal(dirname)) {
			if(badlist) {
				badlist->push_back(new_prefix + "/");
			} else {
				return false;
			}
		}

		// Recurse into subdir.
		if(!check_names_legal_internal(path, new_prefix, badlist) && !badlist) {
			return false;
		}
	}

	return badlist ? badlist->empty() : true;
}

bool check_case_insensitive_duplicates_internal(const config& dir, const std::string& prefix, std::vector<std::string>* badlist){
	typedef std::pair<bool, std::string> printed_and_original;
	std::map<std::string, printed_and_original> filenames;
	bool inserted;
	bool printed;
	std::string original;
	for (const config &path : dir.child_range("file")) {
		const config::attribute_value &filename = path["name"];
		const std::string lowercase = boost::algorithm::to_lower_copy(filename.str(), std::locale::classic());
		const std::string with_prefix = prefix + filename.str();
		std::tie(std::ignore, inserted) = filenames.emplace(lowercase, std::make_pair(false, with_prefix));
		if (!inserted){
			if(badlist){
				std::tie(printed, original) = filenames[lowercase];
				if(!printed){
					badlist->push_back(original);
					filenames[lowercase] = make_pair(true, std::string());
				}
				badlist->push_back(with_prefix);
			} else {
				return false;
			}
		}
	}
	for (const config &path : dir.child_range("dir")) {
		const config::attribute_value &filename = path["name"];
		const std::string lowercase = boost::algorithm::to_lower_copy(filename.str(), std::locale::classic());
		const std::string with_prefix = prefix + filename.str();
		std::tie(std::ignore, inserted) = filenames.emplace(lowercase, std::make_pair(false, with_prefix));
		if (!inserted) {
			if(badlist){
				std::tie(printed, original) = filenames[lowercase];
				if(!printed){
					badlist->push_back(original);
					filenames[lowercase] = make_pair(true, std::string());
				}
				badlist->push_back(with_prefix);
			} else {
				return false;
			}
		}
		if (!check_case_insensitive_duplicates_internal(path, prefix + filename + "/", badlist) && !badlist){
			return false;
		}
	}

	return badlist ? badlist->empty() : true;
}

} // end unnamed namespace 3

bool check_names_legal(const config& dir, std::vector<std::string>* badlist)
{
	// Usually our caller is passing us the root [dir] for an add-on, which
	// shall contain a single subdir named after the add-on itself, so we can
	// start with an empty display prefix and that'll reflect the addon
	// structure correctly (e.g. "Addon_Name/~illegalfilename1").
	return check_names_legal_internal(dir, "", badlist);
}

bool check_case_insensitive_duplicates(const config& dir, std::vector<std::string>* badlist){
    return check_case_insensitive_duplicates_internal(dir, "", badlist);
}

ADDON_TYPE get_addon_type(const std::string& str)
{
	if (str.empty())
		return ADDON_UNKNOWN;

	unsigned addon_type_num = 0;

	while(++addon_type_num != ADDON_TYPES_COUNT) {
		if(str == addon_type_strings[addon_type_num])  {
			return ADDON_TYPE(addon_type_num);
		}
	}

	return ADDON_UNKNOWN;
}

std::string get_addon_type_string(ADDON_TYPE type)
{
	assert(type != ADDON_TYPES_COUNT);
	return addon_type_strings[type];
}

namespace {
	const char escape_char = '\x01'; /**< Binary escape char. */
} // end unnamed namespace 2

bool needs_escaping(char c) {
	switch(c) {
		case '\x00':
		case escape_char:
		case '\x0D': //Windows -- carriage return
		case '\xFE': //Parser code -- textdomain or linenumber&filename
			return true;
		default:
			return false;
	}
}

std::string encode_binary(const std::string& str)
{
	std::string res;
	res.resize(str.size());
	std::size_t n = 0;
	for(std::string::const_iterator j = str.begin(); j != str.end(); ++j) {
		if(needs_escaping(*j)) {
			res.resize(res.size()+1);
			res[n++] = escape_char;
			res[n++] = *j + 1;
		} else {
			res[n++] = *j;
		}
	}

	return res;
}

std::string unencode_binary(const std::string& str)
{
	std::string res(str.size(), '\0');

	std::size_t n = 0;
	for(std::string::const_iterator j = str.begin(); j != str.end(); ) {
		char c = *j++;
		if((c == escape_char) && (j != str.end())) {
			c = (*j++) - 1;
		}
		res[n++] = c;
	}

	res.resize(n);
	return res;
}

static std::string file_hash_raw(const config& file)
{
	return utils::md5(file["contents"].str()).base64_digest();
}

std::string file_hash(const config& file)
{
	std::string hash = file["hash"].str();
	if(hash.empty()) {
		hash = file_hash_raw(file);
	}
	return hash;
}

bool comp_file_hash(const config& file_a, const config& file_b)
{
	return file_a["name"] == file_b["name"] && file_hash(file_a) == file_hash(file_b);
}

void write_hashlist(config& hashlist, const config& data)
{
	hashlist["name"] = data["name"];

	for(const config& f : data.child_range("file")) {
		config& file = hashlist.add_child("file");
		file["name"] = f["name"];
		file["hash"] = file_hash_raw(f);
	}

	for(const config& d : data.child_range("dir")) {
		config& dir = hashlist.add_child("dir");
		write_hashlist(dir, d);
	}
}

bool contains_hashlist(const config& from, const config& to)
{
	for(const config& f : to.child_range("file")) {
		bool found = false;
		for(const config& d : from.child_range("file")) {
			found |= comp_file_hash(f, d);
			if(found)
				break;
		}
		if(!found) {
			return false;
		}
	}

	for(const config& d : to.child_range("dir")) {
		const config& origin_dir = from.find_child("dir", "name", d["name"]);
		if(origin_dir) {
			if(!contains_hashlist(origin_dir, d)) {
				return false;
			}
		} else {
			// The case of empty new subdirectories
			const config dummy_dir = config("name", d["name"]);
			if(!contains_hashlist(dummy_dir, d)) {
				return false;
			}
		}
	}

	return true;
}

//! Surround with [dir][/dir]
static bool write_difference(config& pack, const config& from, const config& to, bool with_content)
{
	pack["name"] = to["name"];
	bool has_changes = false;

	for(const config& f : to.child_range("file")) {
		bool found = false;
		for(const config& d : from.child_range("file")) {
			found |= comp_file_hash(f, d);
			if(found)
				break;
		}
		if(!found) {
			config& file = pack.add_child("file");
			file["name"] = f["name"];
			if(with_content) {
				file["contents"] = f["contents"];
				file["hash"] = file_hash(f);
			}
			has_changes = true;
		}
	}

	for(const config& d : to.child_range("dir")) {
		const config& origin_dir = from.find_child("dir", "name", d["name"]);
		config dir;
		if(origin_dir) {
			if(write_difference(dir, origin_dir, d, with_content)) {
				pack.add_child("dir", dir);
				has_changes = true;
			}
		} else {
			const config dummy_dir = config("name", d["name"]);
			if(write_difference(dir, dummy_dir, d, with_content)) {
				pack.add_child("dir", dir);
				has_changes = true;
			}
		}
	}

	return has_changes;
}

/**
 * &from, &to are the top directories of their structures; addlist/removelist tag is treated as [dir]
 *
 * Does it worth it to archive and write the pack on the fly using config_writer?
 * #TODO: clientside verification?
 */
void make_updatepack(config& pack, const config& from, const config& to)
{
	config& removelist = pack.add_child("removelist");
	write_difference(removelist, to, from, false);
	config& addlist = pack.add_child("addlist");
	write_difference(addlist, from, to, true);
}
