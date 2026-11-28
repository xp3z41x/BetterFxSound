/*
FxSound
Copyright (C) 2026  FxSound LLC

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

#pragma once

#include <JuceHeader.h>
#include "AudioPassthru.h"
#include "Settings.h"

namespace FxSound
{
    struct DeviceConfig
    {
        juce::String device_id;
        juce::String device_name;
        juce::String preset;
        juce::String device_form_factor;

        static void initDeviceConfigs(Settings& settings, std::vector<SoundDevice>& sound_devices);
        static void updateDeviceConfigs(Settings& settings, const std::vector<SoundDevice>& sound_devices);

        static juce::Array<DeviceConfig> loadDeviceConfigs(Settings& settings, juce::StringRef key);
        static void saveDeviceConfigs(Settings& settings, juce::StringRef key, const juce::Array<DeviceConfig>& device_configs);

        static DeviceConfig getDeviceConfig(Settings& settings, const juce::String& device_name);
        // BetterFxSound: rows are keyed by endpoint id. The friendly name only finds a row that has no id.
        static DeviceConfig getDeviceConfig(Settings& settings, const juce::String& device_id, const juce::String& device_name);
        static int indexOf(const juce::Array<DeviceConfig>& device_configs, const juce::String& device_id, const juce::String& device_name);
        // The row of this device: by id (or name, see indexOf()), else the row of the same device under an endpoint id
        // Windows no longer lists (see updateDeviceConfigs()). -1 if it has none. sound_devices: every endpoint listed.
        static int findDeviceConfigIndex(const juce::Array<DeviceConfig>& device_configs, const SoundDevice& sound_device,
                                         const std::vector<SoundDevice>& sound_devices);
        // A device usable as an output (real, active, stereo or more): only these are learned as devices.
        static bool qualifiesAsOutput(const SoundDevice& sound_device);

        static std::function<void()> onDeviceConfigsUpdate;

    private:
        static juce::var toJson(const DeviceConfig& device_config);
        static DeviceConfig fromJson(const juce::var& v);
        static juce::Array<DeviceConfig> removeDuplicates(const juce::Array<DeviceConfig>& device_configs);
        static void notifyDeviceConfigsUpdate();
    };
}

