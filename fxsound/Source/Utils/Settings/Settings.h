/*
FxSound
Copyright (C) 2025  FxSound LLC

Contributors:
	www.theremino.com (2025)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef FXSOUND_SETTINGS_H
#define FXSOUND_SETTINGS_H

#include <JuceHeader.h>

namespace FxSound
{
	constexpr wchar_t APPLICATION_NAME[] = L"FxSound";
	constexpr wchar_t SETTINGS_FOLDER[] = L"FxSound";
	constexpr wchar_t SETTINGS_EXTN[] = L"settings";
	constexpr wchar_t SECURE_EXTN[] = L"secure";

	// BetterFxSound: an AsyncUpdater so that a change is written on the next message loop turn (see flush()).
	class Settings final : private juce::AsyncUpdater
	{
	public:
		Settings();
		~Settings();

		String getString(StringRef key) noexcept;
		int    getInt(StringRef key, int default_value=0) noexcept;
		double getDouble(StringRef key) noexcept;
		bool   getBool(StringRef key, bool default_value=false) noexcept;
		juce::var getJson(juce::StringRef key) noexcept;

		void setString(StringRef key, String value, bool default_value=false) noexcept;
		void setInt(StringRef key, int value, bool default_value=false) noexcept;
		void setDouble(StringRef key, double value, bool default_value=false) noexcept;
		void setBool(StringRef key, bool value, bool default_value=false) noexcept;
		void setJson(juce::StringRef key, const juce::var& json) noexcept;

		// True if the key is stored in the user settings (the defaults are not checked).
		bool   containsKey(juce::StringRef key) noexcept;
		// Removes the key from the user settings.
		void   removeValue(juce::StringRef key) noexcept;

		// BetterFxSound: writes pending changes to disk now. JUCE's own save timer fires 3 s after a change, and
		// never once the message loop has ended (quit, restart, logoff), so those paths call this. Every change to the
		// user settings also schedules this on the message thread, so it is on disk by the next
		// message loop turn rather than 3 s later: a crash (whose handler cannot flush safely) and the relaunch after
		// it no longer lose it. It runs outside any caller's lock, and the changes of one turn are written once.
		void   flush() noexcept;

	private:
		void handleAsyncUpdate() override;

		ApplicationProperties app_default_properties_;
		ApplicationProperties app_user_properties_;
		ApplicationProperties app_secure_properties_;
		PropertySet default_settings_;
		PropertiesFile* user_settings_;
	};
}

#endif