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

#include "DeviceConfig.h"
#include "Settings.h"

namespace FxSound
{
    std::function<void()> DeviceConfig::onDeviceConfigsUpdate;

    void DeviceConfig::initDeviceConfigs(Settings& settings, std::vector<SoundDevice>& sound_devices)
    {
        juce::Array<DeviceConfig> device_configs;

        std::sort(sound_devices.begin(), sound_devices.end(),
            [](const SoundDevice& a, const SoundDevice& b)
            {
                return a.isActive > b.isActive;
            });

        std::sort(sound_devices.begin(), sound_devices.end(),
            [](const SoundDevice& a, const SoundDevice& b)
            {
                return (a.isDefaultDevice || a.isTargetedRealPlaybackDevice) > (b.isDefaultDevice || b.isTargetedRealPlaybackDevice);
            });

        for (auto sound_device : sound_devices)
        {
            if (sound_device.isRealDevice)
            {
                DeviceConfig device_config = { sound_device.pwszID.c_str(), sound_device.deviceFriendlyName.c_str(), "", sound_device.deviceFormFactor.c_str() };
                device_configs.add(device_config);
            }
        }

        saveDeviceConfigs(settings, "device_configs", device_configs);
    }

    void DeviceConfig::updateDeviceConfigs(Settings& settings, const std::vector<SoundDevice>& sound_devices)
    {
        juce::Array<DeviceConfig> device_configs = loadDeviceConfigs(settings, "device_configs");
        auto prioritize_new_output = settings.getBool("prioritize_new_output");

        bool save_config = false;
        for (const auto& sound_device : sound_devices)
        {
            // BetterFxSound: a device is learned once it can be an output. One first listed while unplugged, or
            // whose format could not be read, is learned (and counts as new) when it becomes usable.
            if (!qualifiesAsOutput(sound_device))
                continue;

            juce::String id(sound_device.pwszID.c_str());
            juce::String name(sound_device.deviceFriendlyName.c_str());

            auto index = findDeviceConfigIndex(device_configs, sound_device, sound_devices);
            if (index < 0)
            {
                save_config = true;
                DeviceConfig device_config = { id, name, "", sound_device.deviceFormFactor.c_str() };
                if (prioritize_new_output)
                {
                    device_configs.insert(0, device_config);
                }
                else
                {
                    device_configs.add(device_config);
                }
                continue;
            }

            // BetterFxSound: a known device keeps its row (priority, preset) when it is renamed, or comes back
            // under a new endpoint id. The row follows its current id and name.
            auto& device_config = device_configs.getReference(index);
            if (device_config.device_id != id || device_config.device_name != name)
            {
                save_config = true;
                device_config.device_id = id;
                device_config.device_name = name;
                if (!sound_device.deviceFormFactor.empty())
                    device_config.device_form_factor = sound_device.deviceFormFactor.c_str();
            }
        }

        if (save_config)
        {
            saveDeviceConfigs(settings, "device_configs", removeDuplicates(device_configs));
            notifyDeviceConfigsUpdate();
            return;
        }

        for (const auto& device_config : device_configs)
        {
            if (std::find_if(sound_devices.begin(), sound_devices.end(),
                [&](const SoundDevice& sound_device) {
                    return device_config.device_id.isNotEmpty()
                        ? device_config.device_id == sound_device.pwszID.c_str()
                        : device_config.device_name == sound_device.deviceFriendlyName.c_str();
                }) == sound_devices.end())
            {
                // BetterFxSound: one refresh covers every absent device.
                notifyDeviceConfigsUpdate();
                break;
            }
        }
    }

    // BetterFxSound: the listener (Output preferences) updates its components, so it only runs on the
    // message thread. From any other thread it is queued, and the listener current at that time is called.
    void DeviceConfig::notifyDeviceConfigsUpdate()
    {
        if (juce::MessageManager::existsAndIsCurrentThread())
        {
            if (onDeviceConfigsUpdate)
                onDeviceConfigsUpdate();
            return;
        }

        juce::MessageManager::callAsync([]() {
            if (onDeviceConfigsUpdate)
                onDeviceConfigsUpdate();
        });
    }

    DeviceConfig DeviceConfig::getDeviceConfig(Settings& settings, const juce::String& device_name)
    {
        juce::Array<DeviceConfig> device_configs = loadDeviceConfigs(settings, "device_configs");

        for (auto device_config : device_configs)
        {
            if (device_config.device_name == device_name)
            {
                return device_config;
            }
        }

        return {};
    }

    DeviceConfig DeviceConfig::getDeviceConfig(Settings& settings, const juce::String& device_id, const juce::String& device_name)
    {
        juce::Array<DeviceConfig> device_configs = loadDeviceConfigs(settings, "device_configs");

        auto index = indexOf(device_configs, device_id, device_name);
        if (index < 0)
            return {};

        return device_configs[index];
    }

    int DeviceConfig::indexOf(const juce::Array<DeviceConfig>& device_configs, const juce::String& device_id, const juce::String& device_name)
    {
        int index = 0;
        if (device_id.isNotEmpty())
        {
            for (const auto& device_config : device_configs)
            {
                if (device_config.device_id == device_id)
                    return index;
                index++;
            }
        }

        index = 0;
        if (device_name.isNotEmpty())
        {
            for (const auto& device_config : device_configs)
            {
                if (device_config.device_id.isEmpty() && device_config.device_name == device_name)
                    return index;
                index++;
            }
        }

        return -1;
    }

    int DeviceConfig::findDeviceConfigIndex(const juce::Array<DeviceConfig>& device_configs, const SoundDevice& sound_device,
                                            const std::vector<SoundDevice>& sound_devices)
    {
        juce::String id(sound_device.pwszID.c_str());
        juce::String name(sound_device.deviceFriendlyName.c_str());
        juce::String form_factor(sound_device.deviceFormFactor.c_str());

        auto index = indexOf(device_configs, id, name);
        if (index >= 0)
            return index;

        // BetterFxSound: the same device under a new endpoint id (another USB port, Bluetooth paired again, driver
        // reinstalled). Only when this is the one usable device with that name whose id is unknown, and exactly one
        // row with that name (and form factor) belongs to an endpoint Windows no longer lists. An endpoint that is
        // still listed (an unplugged twin) keeps its row.
        int unknown = 0;
        for (const auto& other : sound_devices)
        {
            if (qualifiesAsOutput(other) && name == other.deviceFriendlyName.c_str() &&
                indexOf(device_configs, juce::String(other.pwszID.c_str()), juce::String()) < 0)
            {
                unknown++;
            }
        }
        if (unknown != 1)
            return -1;

        int found = -1;
        index = 0;
        for (const auto& device_config : device_configs)
        {
            const int row = index++;
            if (device_config.device_id.isEmpty() || device_config.device_name != name)
                continue;
            if (device_config.device_form_factor.isNotEmpty() && form_factor.isNotEmpty() && device_config.device_form_factor != form_factor)
                continue;

            bool listed = false;
            for (const auto& other : sound_devices)
            {
                if (device_config.device_id == other.pwszID.c_str())
                {
                    listed = true;
                    break;
                }
            }
            if (listed)
                continue;

            if (found >= 0)
                return -1;  // Ambiguous: keep both rows, learn the device as new
            found = row;
        }

        return found;
    }

    bool DeviceConfig::qualifiesAsOutput(const SoundDevice& sound_device)
    {
        return sound_device.isRealDevice && sound_device.isActive && sound_device.deviceNumChannel >= 2;
    }

    juce::var DeviceConfig::toJson(const DeviceConfig& device_config)
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("device_id", device_config.device_id);
        obj->setProperty("device_name", device_config.device_name);
        obj->setProperty("preset", device_config.preset);
        obj->setProperty("device_form_factor", device_config.device_form_factor);

        return juce::var(obj);
    }

    DeviceConfig DeviceConfig::fromJson(const juce::var& v)
    {
        DeviceConfig device_config;

        if (auto* obj = v.getDynamicObject())
        {
            device_config.device_id = obj->getProperty("device_id").toString();
            device_config.device_name = obj->getProperty("device_name").toString();
            device_config.preset = obj->getProperty("preset").toString();
            device_config.device_form_factor = obj->getProperty("device_form_factor").toString();
        }

        return device_config;
    }

    juce::Array<DeviceConfig> DeviceConfig::removeDuplicates(const juce::Array<DeviceConfig>& device_configs)
    {
        // BetterFxSound: one row per endpoint id, so two devices with the same name (identical monitors or DACs)
        // each keep their own. Only a row with no id is matched by name.
        juce::Array<DeviceConfig> result;
        juce::StringArray seen_keys;

        for (const auto& device_config : device_configs)
        {
            auto key = device_config.device_id.isNotEmpty() ? "id:" + device_config.device_id : "name:" + device_config.device_name;
            if (!seen_keys.contains(key))
            {
                seen_keys.add(key);
                result.add(device_config);
            }
        }

        return result;
    }

    juce::Array<DeviceConfig> DeviceConfig::loadDeviceConfigs(Settings& settings, juce::StringRef key)
    {
        juce::Array<DeviceConfig> device_configs;

        auto json = settings.getJson(key);
        if (!json.isArray())
            return device_configs;

        const auto& jsonArray = *json.getArray();

        for (const auto& device_config : jsonArray)
        {
            device_configs.add(fromJson(device_config));
        }

        return removeDuplicates(device_configs);
    }

    void DeviceConfig::saveDeviceConfigs(Settings& settings, juce::StringRef key, const juce::Array<DeviceConfig>& device_configs)
    {
        juce::Array<juce::var> jsonArray;
        jsonArray.ensureStorageAllocated(device_configs.size());

        for (const auto& device_config : device_configs)
        {
            jsonArray.add(toJson(device_config));
        }

        settings.setJson(key, juce::var(jsonArray));
    }
}