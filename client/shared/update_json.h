#pragma once

#include <string>

// Returns the string value of the next `key` at or after *pos and moves *pos past it.
// *pos becomes npos when the key isn't found. A non-string value (null) gives "".
inline std::string JsonString(const std::string& json, const char* key, size_t* pos = nullptr)
{
	size_t i = json.find(std::string("\"") + key + "\"", pos ? *pos : 0);
	if (pos)
		*pos = i;
	if (i == std::string::npos)
		return "";
	i = json.find_first_not_of(" \t\r\n", json.find(':', i) + 1);
	if (i == std::string::npos || json[i] != '"')
	{
		if (pos)
			*pos = i;
		return "";
	}
	std::string out;
	for (i++; i < json.size() && json[i] != '"'; i++)
	{
		char c = json[i];
		if (c == '\\' && i + 1 < json.size())
		{
			c = json[++i];
			if (c == 'n') c = '\n';
			else if (c == 'r') continue;
			else if (c == 't') c = ' ';
			else if (c == 'u') { i += 4; c = '?'; }
		}
		out += c;
	}
	if (pos)
		*pos = i;
	return out;
}

// Takes the /releases list (newest first) and returns the newest tag, or "" when `current`
// is already the newest. `log` gets the notes of every release newer than `current`.
inline std::string Changelog(const std::string& json, const std::string& current, std::string& log)
{
	std::string latest;
	size_t pos = 0;
	for (;;)
	{
		const std::string tag = JsonString(json, "tag_name", &pos);
		if (pos == std::string::npos || tag == current)
			break;
		JsonString(json, "prerelease", &pos);
		const bool prerelease = pos != std::string::npos && json.compare(pos, 4, "true") == 0;
		// The workflow puts the notes first, then a --- line and the install text.
		std::string notes = JsonString(json, "body", &pos);
		if (pos == std::string::npos)
			break;
		if (prerelease)
			continue;
		notes = notes.substr(0, notes.find("\n---"));
		notes.erase(notes.find_last_not_of(" \n") + 1);
		if (notes.empty())
			notes = "(no notes)";
		if (latest.empty())
			latest = tag;
		log += tag + ":\n" + notes + "\n\n";
	}
	log.erase(log.find_last_not_of('\n') + 1);
	if (log.size() > 1500)
		log = log.substr(0, 1500) + "...";
	return latest;
}
