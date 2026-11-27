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

#include "FxController.h"
#include <algorithm>
#include "FxMainWindow.h"
#include "FxSystemTrayView.h"
#include "FxMessage.h"
#include "FxLanguage.h"
#include "../Utils/SysInfo/SysInfo.h"
#include <iostream>
#include <cstdio>

class FxDeviceErrorMessage : public FxWindow
{
public:
	FxDeviceErrorMessage()
	{
		setContent(&message_content_);
		centreWithSize(getWidth(), getHeight());
		addToDesktop(0);
		setAlwaysOnTop(true);
	}
	~FxDeviceErrorMessage() = default;

	void closeButtonPressed() override
	{
		exitModalState(0);
		removeFromDesktop();
	}

private:
	class MessageComponent : public Component
	{
	public:
		MessageComponent()
		{
			auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

			error_message_.setFont(theme.getNormalFont());
			contact_message_.setFont(theme.getNormalFont());

			error_message_.setText(TRANS("Oops! There's an issue with your playback device settings.\r\nBefore we can get started, please go through the "), NotificationType::dontSendNotification);
			error_message_.setJustificationType(Justification::topLeft);
			addAndMakeVisible(error_message_);

			error_link_.setButtonText(TRANS("troubleshooting steps here."));
			error_link_.setURL(URL(L"https://www.fxsound.com/learning-center/installation-troubleshooting"));
			error_link_.setJustificationType(Justification::topLeft);
			addAndMakeVisible(error_link_);

			contact_message_.setText(TRANS(" if you're still having problems."), NotificationType::dontSendNotification);
			contact_message_.setJustificationType(Justification::topLeft);
			addAndMakeVisible(contact_message_);

			contact_link_.setButtonText(TRANS("Contact us"));
			contact_link_.setURL(URL(L"https://www.fxsound.com/support"));
			contact_link_.setJustificationType(Justification::topLeft);
			addAndMakeVisible(contact_link_);

			setSize(WIDTH, HEIGHT);
		}
		~MessageComponent() = default;

	private:
		static constexpr int WIDTH = 400;
		static constexpr int HEIGHT = 142;
		static constexpr int MESSAGE_HEIGHT = (24 + 2) * 2;
		static constexpr int HYPERLINK_HEIGHT = 24;

		void resized() override
		{
			auto bounds = getLocalBounds();
			bounds.setTop(10);
			bounds.reduce(20, 0);

			RectanglePlacement placement(RectanglePlacement::xLeft
				| RectanglePlacement::yTop
				| RectanglePlacement::doNotResize);

			auto component_area = juce::Rectangle<int>(0, 0, bounds.getWidth(), MESSAGE_HEIGHT);
			error_message_.setBounds(placement.appliedTo(component_area, bounds));
			auto x = error_message_.getX() + error_message_.getBorderSize().getLeft();

			bounds.setTop(error_message_.getBottom() + 2);
			component_area = juce::Rectangle<int>(0, 0, bounds.getWidth(), HYPERLINK_HEIGHT);
			error_link_.setBounds(placement.appliedTo(component_area, bounds).withX(x));

			bounds.setTop(error_link_.getBottom() + 20);
			component_area = juce::Rectangle<int>(0, 0, contact_link_.getTextWidth(), HYPERLINK_HEIGHT);
			contact_link_.setBounds(placement.appliedTo(component_area, bounds).withX(x));

			bounds = juce::Rectangle<int>(contact_link_.getRight(), contact_link_.getY(), error_message_.getWidth() - contact_link_.getWidth(), MESSAGE_HEIGHT / 2);
			contact_message_.setBounds(bounds);
		}

		Label error_message_;
		Label contact_message_;
		FxHyperlink error_link_;
		FxHyperlink contact_link_;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MessageComponent)
	};

	MessageComponent message_content_;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxDeviceErrorMessage)
};

FxController::FxController() : message_window_(L"FxSoundHotkeys", (WNDPROC)eventCallback)
{
	dfx_enabled_ = true;
	authenticated_ = true;
	minimize_tip_ = true;

	hotkeys_registered_ = false;
	output_changed_ = false;
	playback_device_available_ = true;
	system_sleeping_ = false;
	powerNotify_ = nullptr;
	unregister_suspend_resume_notification_ = nullptr;

	device_count_ = 0;
	output_device_name_ = L"";
	no_output_ = false;
	clearOutputRestore();

	audio_process_time_ = 0;
	audio_process_on_counter_ = 0;
	audio_process_off_counter_ = 0;
	audio_process_on_ = false;

	audio_process_start_time_ = -1LL;

	preset_dirty_ = false;
	auto_save_counter_ = 0;

	main_window_ = nullptr;
	audio_passthru_ = nullptr;

	file_logger_.reset(FileLogger::createDefaultAppLogger(L"FxSound", L"fxsound.log", L"FxSound logs"));
	logMessage("v" + JUCEApplication::getInstance()->getApplicationVersion());
	logMessage(SystemStats::getOperatingSystemName());

	SYSTEM_INFO sys_info;
	GetNativeSystemInfo(&sys_info);
	if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL)
	{
		logMessage(String("x86"));
	}
	else if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64)
	{
		logMessage(String("x64"));
	}
	else if (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64)
	{
		logMessage(String("ARM64"));
	}

    gpu_render_ = settings_.getBool("gpu_render") && canSupportGpuRendering();

	auto view = settings_.getInt("view");
	if (view <= 0 || view > 2)
	{
		view_ = ViewType::Pro;
	}
	else
	{
		view_ = static_cast<ViewType>(view);
	}
	auto hotkeys_support = settings_.getBool("hotkeys") && SysInfo::canSupportHotkeys();
	FxModel::getModel().setHotkeySupport(hotkeys_support);
	if (hotkeys_support)
	{
		registerHotkeys();
	}
	FxModel::getModel().setMenuClicked(settings_.getBool("menu_clicked"));

	always_on_top_ = settings_.getBool("always_on_top");
	hide_help_tooltips_ = settings_.getBool("hide_help_tooltips");
	hide_notifications_ = settings_.getBool("hide_notifications");
	auto_updates_ = settings_.getBool("automatic_updates", true);
	max_user_presets_ = settings_.getInt("max_user_presets");
	if (max_user_presets_ < 10 || max_user_presets_ > 120)
	{
		settings_.setInt("max_user_presets", 120);
		max_user_presets_ = 120;
	}

	SetWindowLongPtr(message_window_.getHandle(), GWLP_USERDATA, (LONG_PTR)this);

	session_id_ = 0;
	ProcessIdToSessionId(GetCurrentProcessId(), &session_id_);
	WTSRegisterSessionNotification(message_window_.getHandle(), NOTIFY_FOR_THIS_SESSION);
}

FxController::~FxController()
{
	if (powerNotify_ != nullptr && unregister_suspend_resume_notification_ != nullptr)
	{
		unregister_suspend_resume_notification_(powerNotify_);
		powerNotify_ = nullptr;
	}
	stopTimer();
	WTSUnRegisterSessionNotification(message_window_.getHandle());
	unregisterHotkeys();
}


void FxController::initConfig(const String& commandline)
{
	auto arg_list = ArgumentList(File::getSpecialLocation(File::SpecialLocationType::invokedExecutableFile).getFileName(), commandline);

	auto power_state = arg_list.getValueForOption("--power");
	auto preset = arg_list.getValueForOption("--preset").unquoted();
	auto view = arg_list.getValueForOption("--view");
	auto output_device = arg_list.getValueForOption("--output").unquoted();
	auto language = arg_list.getValueForOption("--language");
	auto numbands = arg_list.getValueForOption("--num_bands");
	auto balance = arg_list.getValueForOption("--balance");
	auto filterq = arg_list.getValueForOption("--filter_q");
	auto mastergain = arg_list.getValueForOption("--master_gain");
	auto volume_leveling = arg_list.getValueForOption("--volume_leveling");

    // Forces GPU rendering for this session only; the saved preference is not changed.
    if (arg_list.containsOption("--gpu") && canSupportGpuRendering())
    {
        gpu_render_ = true;
    }

	if (arg_list.containsOption("--run_minimized"))
	{
		settings_.setBool("run_minimized", true);
	}

	if (power_state.isNotEmpty())
	{
		if (power_state.getIntValue() == 0)
			settings_.setBool("power", false);
		else
			settings_.setBool("power", true);
	}

	if (preset.isNotEmpty())
	{
		settings_.setString("preset", preset);
	}

	if (output_device.isNotEmpty())
	{
		// Applied by initOutputs(), once the devices are known: enable only this output.
		startup_output_name_ = output_device;
	}

	if (view.isNotEmpty())
	{
		auto value = view.getIntValue();
		if (value == ViewType::Lite || value == ViewType::Pro)
		{
			settings_.setInt("view", value);
			view_ = static_cast<ViewType>(value);
		}
	}

	if (language.isEmpty())
	{
		language = settings_.getString("language");
		if (language.isEmpty())
		{
			language = SystemStats::getDisplayLanguage();
		}
	}

	setLanguage(language);

	// ------------------------------------------------------------------------------------------
	//  NumBands / Balance / FilterQ / MasterGain  / Volume Leveling - SET ON START
	// ------------------------------------------------------------------------------------------
	int nb = 10;
	if (numbands == "")
	{
		nb = settings_.getInt("num_bands");
	}
	else
	{
		nb = numbands.getIntValue();
	}
	if (nb != 5 && nb != 10 && nb != 15 && nb != 20 && nb != 31) nb = DEFAULT_NUM_EQ_BANDS;
	setNumEqBands(nb);

	float vl = 0;
	if (volume_leveling.isEmpty())
	{
		vl = static_cast<float>(settings_.getDouble("volume_leveling"));
	}
	else
	{
		vl = volume_leveling.getFloatValue();
	}
	if (vl < 0 || vl > 4) vl = DEFAULT_VOLUME_LEVELING;
	setVolumeLeveling(vl);

	float bl = 0;
	if (balance == "")
	{
		bl = static_cast<float>(settings_.getDouble("balance"));
	}
	else
	{
		bl = balance.getFloatValue();
	}
	if (bl < -20 || bl > +20) bl = DEFAULT_BALANCE;
	setBalance(bl);

	float fq = 0;
	if (filterq == "")
	{
		fq = static_cast<float>(settings_.getDouble("filter_q"));
	}
	else
	{
		fq = filterq.getFloatValue();
	}
	if (fq < 1 || fq > 3) fq = DEFAULT_FILTER_Q;
	setFilterQ(fq);

	float mg = 0;
	if (mastergain == "")
	{
		mg = static_cast<float>(settings_.getDouble("master_gain"));
	}
	else
	{
		mg = mastergain.getFloatValue();
	}
	if (mg < -20 || mg > +20) mg = DEFAULT_MASTER_GAIN;
	setMasterGain(mg);
}

void FxController::applyConfig(const String& commandline)
{
	auto arg_list = ArgumentList(File::getSpecialLocation(File::SpecialLocationType::invokedExecutableFile).getFileName(), commandline);

	if (arg_list.containsOption("--status"))
	{
		printStatus();
		return;
	}

	auto power_state = arg_list.getValueForOption("--power");
	auto view = arg_list.getValueForOption("--view");
	auto output_device = arg_list.getValueForOption("--output").unquoted();
	auto enable_output = arg_list.getValueForOption("--enable-output").unquoted();
	auto disable_output = arg_list.getValueForOption("--disable-output").unquoted();
	auto output_volume = arg_list.getValueForOption("--output-volume").unquoted();
	auto output_delay = arg_list.getValueForOption("--output-delay").unquoted();
	auto language = arg_list.getValueForOption("--language");
	auto numbands = arg_list.getValueForOption("--num_bands");
	auto balance = arg_list.getValueForOption("--balance");
	auto filterq = arg_list.getValueForOption("--filter_q");
	auto mastergain = arg_list.getValueForOption("--master_gain");
	auto volume_leveling = arg_list.getValueForOption("--volume_leveling");
	auto band_freq = arg_list.getValueForOption("--set_band_freq").unquoted();
	auto band_gain = arg_list.getValueForOption("--set_band_gain").unquoted();
	auto effect = arg_list.getValueForOption("--set_effect").unquoted();

	if (power_state.isNotEmpty() && !SysInfo::isRemoteSession())
	{
		if (power_state.getIntValue() == 0)
			setPowerState(false);
		else
			setPowerState(true);
	}

	auto& model = FxModel::getModel();

	auto sanitizePresetName = [&model](const String& name) -> String {
		String preset_name = name.removeCharacters("<>:\"/\\|?*");

		if (preset_name.length() > 64)
		{
			preset_name = preset_name.substring(0, 64);
		}

		if (preset_name.isNotEmpty() && model.isPresetNameValid(preset_name))
		{
			return preset_name;
		}

		return "";
    };

	if (model.getPowerState())
	{
		if (arg_list.containsOption("--preset"))
		{
			if (auto preset_name = arg_list.getValueForOption("--preset").unquoted(); preset_name.isNotEmpty())
			{
				setPreset(preset_name);
			}
		}
		else if (arg_list.containsOption("--save_preset"))
		{
			auto arg_val = arg_list.getValueForOption("--save_preset").unquoted();
			if (auto preset_name = sanitizePresetName(arg_val); preset_name.isNotEmpty())
			{
				if (model.isPresetModified() && model.getUserPresetCount() < getMaxUserPresets())
				{
					savePreset(preset_name);
				}
			}
		}
		else if (arg_list.containsOption("--overwrite_preset"))
		{
			auto preset = model.getPreset(model.getSelectedPreset());
			if (model.isPresetModified() && preset.type == FxModel::PresetType::UserPreset)
			{
				savePreset();
			}
		}
		else if (arg_list.containsOption("--undo_preset"))
		{
			if (model.isPresetModified())
			{
                undoPreset();
			}
        }
		else if (arg_list.containsOption("--rename_preset"))
		{
			auto arg_val = arg_list.getValueForOption("--rename_preset").unquoted();

			if (auto preset_name = sanitizePresetName(arg_val); preset_name.isNotEmpty())
			{
				auto preset = model.getPreset(model.getSelectedPreset());
				if (!model.isPresetModified() && preset.type == FxModel::PresetType::UserPreset)
				{
					renamePreset(preset_name);
				}
			}
		}
		else if (arg_list.containsOption("--delete_preset"))
		{
			auto preset = model.getPreset(model.getSelectedPreset());
			if (preset.type == FxModel::PresetType::UserPreset)
			{
				deletePreset();
			}
		}
	}

	if (output_device.isNotEmpty())
	{
		// BetterFxSound outputs rework: --output enables only this output.
		for (auto& output : getOutputs())
		{
			if (output.present && output.name == output_device)
			{
				enableOnlyOutput(output.id);
				model.notifyOutputListUpdated();
				break;
			}
		}
	}

	// BetterFxSound outputs rework: per-output options. Outputs are matched by friendly name, as with
	// --output. A connected output wins over a disconnected one with the same name. Enabling needs the
	// device to be connected; the other options also work on an enabled output that is not connected.
	auto findOutputByName = [this](const String& name, String& id) -> bool
	{
		bool found = false;
		for (auto& output : getOutputs())
		{
			if (output.name == name && (output.present || !found))
			{
				id = output.id;
				found = true;
				if (output.present)
					break;
			}
		}
		return found;
	};

	// "<name>:<number>": the name may contain ':', so split on the last one.
	auto parseNameAndValue = [](const String& arg, String& name, int& value) -> bool
	{
		auto colon = arg.lastIndexOfChar(':');
		if (colon <= 0)
			return false;
		auto number = arg.substring(colon + 1).trim();
		if (number.isEmpty() || number.length() > 4 || !number.containsOnly("0123456789"))
			return false;
		name = arg.substring(0, colon);
		// --output-volume="<name>":<number> (quotes around the name only, as typed in cmd.exe)
		// arrives here as <name>":<number>, its opening quote already removed: drop the closing one.
		if (name.indexOfChar('"') == name.length() - 1)
			name = name.dropLastCharacters(1);
		value = number.getIntValue();
		return true;
	};

	String output_id;
	bool outputs_touched = false;
	if (enable_output.isNotEmpty() && findOutputByName(enable_output, output_id))
	{
		setOutputEnabled(output_id, true);
		outputs_touched = true;
	}
	if (disable_output.isNotEmpty() && findOutputByName(disable_output, output_id))
	{
		setOutputEnabled(output_id, false);
		outputs_touched = true;
	}

	String output_arg_name;
	int output_arg_value = 0;
	if (output_volume.isNotEmpty() && parseNameAndValue(output_volume, output_arg_name, output_arg_value) &&
		output_arg_value <= 100 && findOutputByName(output_arg_name, output_id))
	{
		setOutputVolume(output_id, output_arg_value);
		outputs_touched = true;
	}
	if (output_delay.isNotEmpty() && parseNameAndValue(output_delay, output_arg_name, output_arg_value) &&
		output_arg_value <= 500 && findOutputByName(output_arg_name, output_id))
	{
		setOutputDelay(output_id, output_arg_value);
		outputs_touched = true;
	}

	// Volume and delay changes do not notify by themselves: refresh any open output panel once.
	if (outputs_touched)
	{
		model.notifyOutputListUpdated();
	}

	// ------------------------------------------------------------------------------------------
	//  NumBands / Balance / FilterQ / MasterGain / Volume Leveling
	// ------------------------------------------------------------------------------------------
	int nb = DEFAULT_NUM_EQ_BANDS;
	if (numbands.isNotEmpty())
	{
		nb = numbands.getIntValue();
		if (nb != 5 && nb != 10 && nb != 15 && nb != 20 && nb != 31) nb = DEFAULT_NUM_EQ_BANDS;
		setNumEqBands(nb);
	}

	float vl = 0;
	if (volume_leveling.isNotEmpty())
	{
		vl = volume_leveling.getFloatValue();
		if (vl < 0 || vl > 4) vl = DEFAULT_VOLUME_LEVELING;
		setVolumeLeveling(vl);
	}

	float bl = 0;
	if (balance.isNotEmpty())
	{
		bl = balance.getFloatValue();
		if (bl < -20 || bl > +20) bl = DEFAULT_BALANCE;
		setBalance(bl);
	}

	float fq = 0;
	if (filterq.isNotEmpty())
	{
		fq = filterq.getFloatValue();
		if (fq < 1 || fq > 3) fq = DEFAULT_FILTER_Q;
		setFilterQ(fq);
	}

	float mg = 0;
	if (mastergain.isNotEmpty())
	{
		mg = mastergain.getFloatValue();
		if (mg < -20 || mg > +20) mg = DEFAULT_MASTER_GAIN;
		setMasterGain(mg);
	}

	if (view.isNotEmpty())
	{
		auto value = view.getIntValue();
		if (value == ViewType::Lite || value == ViewType::Pro)
		{
			settings_.setInt("view", value);
			view_ = static_cast<ViewType>(value);
		}
		showView();
	}

	if (language.isNotEmpty())
	{
		setLanguage(language);
	}

	if (arg_list.containsOption("--run_minimized"))
	{
		settings_.setBool("run_minimized", true);
		hideMainWindow();
	}
	else
	{
		showMainWindow();
	}

	// ------------------------------------------------------------------------------------------
	//  Band frequency / gain / effect values - "<index>:<value>[,<index>:<value>...]"
	// ------------------------------------------------------------------------------------------
	if (band_freq.isNotEmpty())
	{
        auto band_frequency_pairs = StringArray::fromTokens(band_freq, ",", "\"");
		if (band_frequency_pairs.size() <= getNumEqBands())
		{
			for (auto& pair : band_frequency_pairs)
			{
				auto band = pair.upToFirstOccurrenceOf(":", false, false).trim().getIntValue();
				auto freq = pair.fromFirstOccurrenceOf(":", false, false).trim().getFloatValue();
				setEqBandFrequency(band, freq);
			}
        }

		if (view_ == ViewType::Pro)
		{
			main_window_->update();
		}
	}

	if (band_gain.isNotEmpty())
	{
		auto band_gain_pairs = StringArray::fromTokens(band_gain, ",", "\"");
		if (band_gain_pairs.size() <= getNumEqBands())
		{
			for (auto& pair : band_gain_pairs)
			{
				auto band = pair.upToFirstOccurrenceOf(":", false, false).trim().getIntValue();
				auto gain = pair.fromFirstOccurrenceOf(":", false, false).trim().getFloatValue();
				setEqBandBoostCut(band, gain);
			}
		}

		if (view_ == ViewType::Pro)
		{
			main_window_->update();
		}
	}

	if (effect.isNotEmpty())
	{
        auto effect_pairs = StringArray::fromTokens(effect, ",", "\"");
		if (effect_pairs.size() <= 5)
		{
			for (auto& pair : StringArray::fromTokens(effect, ",", "\""))
			{
				auto name = pair.upToFirstOccurrenceOf(":", false, false).trim().toLowerCase();
				auto value = pair.fromFirstOccurrenceOf(":", false, false).trim().getFloatValue();

				if (name == "fidelity" || name == "clarity")
					setEffectValue(FxEffects::Fidelity, value);
				else if (name == "ambience")
					setEffectValue(FxEffects::Ambience, value);
				else if (name == "surround")
					setEffectValue(FxEffects::Surround, value);
				else if (name == "dynamicboost" || name == "dynamic_boost")
					setEffectValue(FxEffects::DynamicBoost, value);
				else if (name == "bass" || name == "bassboost" || name == "bass_boost")
					setEffectValue(FxEffects::Bass, value);
			}

			if (view_ == ViewType::Pro)
			{
				main_window_->update();
			}
		}
	}
}

File FxController::getStatusFile()
{
	return File::getSpecialLocation(File::userApplicationDataDirectory).getChildFile("FxSound").getChildFile("status.json");
}

void FxController::printStatus()
{
	auto& model = FxModel::getModel();

	auto* settings = new DynamicObject();
	settings->setProperty("version", ProjectInfo::versionString);
	settings->setProperty("power", model.getPowerState());

	Array<var> built_in_presets, user_presets;
	for (int i = 0; i < model.getPresetCount(); i++)
	{
		auto preset = model.getPreset(i);

		auto* preset_obj = new DynamicObject();
		preset_obj->setProperty("name", preset.name);
		preset_obj->setProperty("modified", preset.modified);

		if (preset.type == FxModel::PresetType::UserPreset)
			user_presets.add(var(preset_obj));
		else
			built_in_presets.add(var(preset_obj));
	}

	auto* presets_obj = new DynamicObject();
	presets_obj->setProperty("built_in", built_in_presets);
	presets_obj->setProperty("user_defined", user_presets);
	settings->setProperty("presets", var(presets_obj));
	settings->setProperty("selected_preset", model.getPreset(model.getSelectedPreset()).name);

	Array<var> output_devices;
	for (auto& device : audio_passthru_->getSoundDevices())
	{
		if (device.isRealDevice)
			output_devices.add(String(device.deviceFriendlyName.c_str()));
	}
	settings->setProperty("output_devices", output_devices);

	// BetterFxSound outputs rework: every output with its own state. selected_output is kept for
	// compatibility: the first enabled output in the list below, or empty when none is enabled.
	Array<var> outputs;
	String first_enabled_output;
	for (auto& output : getOutputs())
	{
		auto* output_obj = new DynamicObject();
		output_obj->setProperty("id", output.id);
		output_obj->setProperty("name", output.name);
		output_obj->setProperty("enabled", output.enabled);
		output_obj->setProperty("present", output.present);
		output_obj->setProperty("volume", output.volume_percent);
		output_obj->setProperty("delay_ms", output.delay_ms);

		const char* channel_mode = "both";
		switch (output.channel_mode)
		{
		case OutputChannelMode::Left: channel_mode = "left"; break;
		case OutputChannelMode::Right: channel_mode = "right"; break;
		case OutputChannelMode::Mono: channel_mode = "mono"; break;
		default: break;
		}
		output_obj->setProperty("channel_mode", String(channel_mode));

		outputs.add(var(output_obj));
		if (output.enabled && first_enabled_output.isEmpty())
			first_enabled_output = output.name;
	}
	settings->setProperty("outputs", outputs);
	settings->setProperty("selected_output", first_enabled_output);

	Array<var> bands;
	for (int i = 0; i < getNumEqBands(); i++)
	{
		auto* band_obj = new DynamicObject();
		band_obj->setProperty("index", i);
		band_obj->setProperty("frequency", getEqBandFrequency(i));
		band_obj->setProperty("gain", getEqBandBoostCut(i));
		bands.add(var(band_obj));
	}

	auto* eq_obj = new DynamicObject();
	eq_obj->setProperty("num_bands", getNumEqBands());
	eq_obj->setProperty("master_gain", getMasterGain());
	eq_obj->setProperty("volume_leveling", getVolumeLeveling());
	eq_obj->setProperty("filter_q", getFilterQ());
	eq_obj->setProperty("balance", getBalance());
	eq_obj->setProperty("bands", bands);
	settings->setProperty("equalizer", var(eq_obj));

	auto* effects_obj = new DynamicObject();
	effects_obj->setProperty("clarity", getEffectValue(FxEffects::Fidelity) * 10.0f);
	effects_obj->setProperty("ambience", getEffectValue(FxEffects::Ambience) * 10.0f);
	effects_obj->setProperty("surround", getEffectValue(FxEffects::Surround) * 10.0f);
	effects_obj->setProperty("dynamicboost", getEffectValue(FxEffects::DynamicBoost) * 10.0f);
	effects_obj->setProperty("bass", getEffectValue(FxEffects::Bass) * 10.0f);
	settings->setProperty("effects", var(effects_obj));

	auto json_text = JSON::toString(var(settings));

	auto status_file = getStatusFile();
	status_file.getParentDirectory().createDirectory();
	status_file.replaceWithText(json_text);

	// Best-effort console output for interactive use. Attaches to this
	// (already-running) instance's own parent console - i.e. whichever
	// console FxSound.exe was originally launched from - not the console of
	// the process that just forwarded this command.
	if (AttachConsole(ATTACH_PARENT_PROCESS))
	{
		FILE* fp = nullptr;
		freopen_s(&fp, "CONOUT$", "w", stdout);

		std::cout << json_text << std::endl;
		std::cout.flush();

		FreeConsole();
	}
}

void FxController::init(FxMainWindow* main_window, FxSystemTrayView* system_tray_view, AudioPassthru* audio_passthru)
{
	if (!isTimerRunning())
	{
		main_window_ = main_window;
		audio_passthru_ = audio_passthru;
		system_tray_view_ = system_tray_view;

		if (audio_passthru_->init() != 0)
		{
			String message(TRANS("Error in system audio configuration. Unable to run FxSound"));
			AlertWindow::showMessageBoxAsync(AlertWindow::AlertIconType::WarningIcon, JUCEApplication::getInstance()->getApplicationName(), message, TRANS("OK"), nullptr,
				ModalCallbackFunction::create([] (int)
				{
					JUCEApplication::getInstance()->systemRequestedQuit();
				}));

			return;
		}

		auto app_version = JUCEApplication::getInstance()->getApplicationVersion();
		auto prev_version = settings_.getString("version");
		if (prev_version != app_version)
		{
			RegDeleteTree(HKEY_CURRENT_USER, L"Software\\DFX");

			FxModel::getModel().pushMessage(" ", { TRANS("Click here to see what's new on this version!"), "https://github.com/xp3z41x/BetterFxSound/releases" });

			settings_.setString("version", app_version);
			settings_.setBool("run_minimized", false);
		}

		audio_passthru_->setDspProcessingModule(&dfx_dsp_);
		output_devices_ = audio_passthru_->getSoundDevices(false);
		initOutputs(output_devices_);

		if (!dfx_enabled_ && !SysInfo::isRemoteSession())
		{
			main_window_->removeFromDesktop();
			auto* error_message = new FxDeviceErrorMessage();
			error_message->enterModalState(true, ModalCallbackFunction::create([](int)
			{
				JUCEApplication::getInstance()->systemRequestedQuit();
			}), true);
			return;
		}

		audio_passthru_->registerCallback(this);

		auto path = File(File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName()) + L"FxSound\\Presets");
		if (!path.exists())
		{
			path.createDirectory();
		}

		setPowerState(settings_.getBool("power"));

		initPresets();

		auto preset_name = settings_.getString("preset");
		setPreset(preset_name);

		showView();

		auto theme_mode = settings_.getInt("theme_mode", 0);
		if (theme_mode < 0 || theme_mode >= static_cast<int>(FxThemeMode::NumModes))
		{
			theme_mode = 0;
		}
		if (FxTheme::getThemeMode() != static_cast<FxThemeMode>(theme_mode))
		{
			FxTheme::setThemeMode(static_cast<FxThemeMode>(theme_mode));
			main_window_->sendLookAndFeelChange();

			auto* theme = dynamic_cast<FxTheme*>(&LookAndFeel::getDefaultLookAndFeel());
			if (theme != nullptr)
			{
				theme->loadFont(language_);
			}
		}

		survey_tip_ = !settings_.getBool("survey_displayed");

		if (!settings_.getBool("run_minimized"))
		{
			showMainWindow();
		}
		else
		{
			hideMainWindow();
		}

		auto power = FxModel::getModel().getPowerState();
		main_window_->setIcon(power, false);
		system_tray_view_->setStatus(power, false);

		HMODULE user32_module = GetModuleHandleW(L"user32.dll");
		if (user32_module != nullptr)
		{
			auto register_suspend_resume_notification = reinterpret_cast<RegisterSuspendResumeNotificationFunc>(
				GetProcAddress(user32_module, "RegisterSuspendResumeNotification"));
			unregister_suspend_resume_notification_ = reinterpret_cast<UnregisterSuspendResumeNotificationFunc>(
				GetProcAddress(user32_module, "UnregisterSuspendResumeNotification"));

			if (register_suspend_resume_notification != nullptr && unregister_suspend_resume_notification_ != nullptr)
			{
				powerNotify_ = register_suspend_resume_notification(message_window_.getHandle(), DEVICE_NOTIFY_WINDOW_HANDLE);
			}
		}
	}
}

String FxController::getAutoSavePath() const
{
	auto data_dir = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName());
	return data_dir + L"FxSound\\AutoSave";
}

String FxController::getAutoSavePresetPath(const String& preset_name) const
{
	return getAutoSavePath() + L"\\" + preset_name + ".fac";
}

void FxController::autoSavePreset(int preset_index)
{
	ScopedLock lock(save_lock_);

	auto preset = FxModel::getModel().getPreset(preset_index);
	if (preset.name.isEmpty())
		return;

	auto auto_save_dir = getAutoSavePath();
	File(auto_save_dir).createDirectory();

	dfx_dsp_.savePreset(preset.name.toWideCharPointer(), auto_save_dir.toWideCharPointer());

	preset_dirty_ = false;
	auto_save_counter_ = 0;
}

void FxController::deleteAutoSavedPreset(const String& preset_name)
{
	File auto_save_file(getAutoSavePresetPath(preset_name));
	if (auto_save_file.existsAsFile())
		auto_save_file.deleteFile();

	preset_dirty_ = false;
	auto_save_counter_ = 0;
}

void FxController::initPresets()
{
	Array<FxModel::Preset> presets;
	auto working_dir = File::getCurrentWorkingDirectory();
	FileSearchPath preset_search_path(File::addTrailingSeparator(working_dir.getFullPathName()) + L"Factsoft");
	auto app_preset_paths = preset_search_path.findChildFiles(File::findFiles, false, "*.fac");
	for (auto path : app_preset_paths)
	{
		auto preset_info = dfx_dsp_.getPresetInfo(path.getFullPathName().toWideCharPointer());
		if (!preset_info.name.empty())
		{
			presets.add({ preset_info.name.c_str(), path.getFullPathName(), FxModel::PresetType::AppPreset });
		}
	}

	auto data_dir = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName());
	FileSearchPath user_preset_search_path(data_dir + L"FxSound\\Presets");
	auto user_preset_paths = user_preset_search_path.findChildFiles(File::findFiles, false, "*.fac");
	for (auto path : user_preset_paths)
	{
		auto preset_info = dfx_dsp_.getPresetInfo(path.getFullPathName().toWideCharPointer());
		if (!preset_info.name.empty())
		{
			presets.add({ preset_info.name.c_str(), path.getFullPathName(), FxModel::PresetType::UserPreset });
		}
	}

	// Mark presets that have an auto-saved version as modified
	for (auto& preset : presets)
	{
		if (File(getAutoSavePresetPath(preset.name)).existsAsFile())
			preset.modified = true;
	}

	FxModel::getModel().initPresets(presets);
}

void FxController::showView()
{
	if (view_ == ViewType::Pro)
	{
		main_window_->showProView();
	}
	else
	{
		main_window_->showLiteView();
	}
}

void FxController::switchView()
{
	if (view_ == ViewType::Pro)
	{
		view_ = ViewType::Lite;
		main_window_->showLiteView();
		settings_.setInt("view", static_cast<int>(view_));
	}
	else
	{
		view_ = ViewType::Pro;
		main_window_->showProView();
		settings_.setInt("view", static_cast<int>(view_));
	}
}

ViewType FxController::getCurrentView()
{
	return view_;
}

void FxController::hideMainWindow()
{
	if (main_window_->isOnDesktop())
	{
		main_window_->removeFromDesktop();
		main_window_->setVisible(false);
		settings_.setBool("run_minimized", true);
	}

	if (minimize_tip_)
	{
		minimize_tip_ = false;
		Timer::callAfterDelay(2000, []() {
			FxModel::getModel().pushMessage(TRANS("FxSound in system tray\r\nClick FxSound icon to reopen"));
			});
	}
}

void FxController::showMainWindow()
{
	if (main_window_ != nullptr)
	{
		settings_.setBool("run_minimized", false);
		main_window_->show();

		auto power = FxModel::getModel().getPowerState();
		main_window_->setIcon(power, audio_process_on_);

		if (survey_tip_)
		{
			int survey_timer = settings_.getInt("survey_timer");
			if (survey_timer == 0)
			{
				survey_timer = static_cast<int>(std::time(nullptr) + (7 * (24 * 60 * 60)));
				settings_.setInt("survey_timer", survey_timer);
			}
			else
			{
				uint64_t current_time = std::time(nullptr);
				if ((int)current_time > survey_timer)
				{
					survey_tip_ = false;
					settings_.setBool("survey_displayed", true);
					String message = TRANS("Thanks for using FxSound! Would you be\r\ninterested in helping us by taking a quick 4 minute\r\nsurvey so we can make FxSound better?");
					if (authenticated_)
					{
						FxModel::getModel().pushMessage(message, { TRANS("Take the survey."), "https://forms.gle/ATx1ayXDWRaMdiR59" });
					}
				}
			}
		}
	}
}

bool FxController::isMainWindowVisible()
{
	if (main_window_ != nullptr)
	{
		return main_window_->isOnDesktop() && main_window_->isVisible();
	}

	return false;
}

void FxController::setMenuClicked(bool clicked)
{
	settings_.setBool("menu_clicked", clicked);
	FxModel::getModel().setMenuClicked(clicked);
}

FxWindow* FxController::getMainWindow()
{
	return main_window_;
}

Point<int> FxController::getSystemTrayWindowPosition(int width, int height)
{
	return system_tray_view_->getSystemTrayWindowPosition(width, height);
}

void FxController::autoSaveModifiedPreset()
{
	auto& model = FxModel::getModel();
	if (model.isPresetModified())
		autoSavePreset(model.getSelectedPreset());
}

bool FxController::exit()
{
	autoSaveModifiedPreset();

	JUCEApplication::getInstance()->systemRequestedQuit();

	return true;
}

void FxController::setPowerState(bool power_state)
{
	if (!dfx_enabled_ || SysInfo::isRemoteSession())
	{
		main_window_->enablePowerButton(false);

		FxModel::getModel().setPowerState(false);
		powerOn(false);

		system_tray_view_->setStatus(false, false);
		main_window_->setIcon(false, false);
		
		return;
    }
	
	main_window_->enablePowerButton(true);

	FxModel::getModel().setPowerState(power_state);	
	powerOn(power_state);
	settings_.setBool("power", power_state);

	system_tray_view_->setStatus(power_state, audio_process_on_);
	main_window_->setIcon(power_state, audio_process_on_);
}

bool FxController::setPreset(const String& preset_name, bool notify)
{
	auto& model = FxModel::getModel();

	auto preset_count = model.getPresetCount();
	for (auto i = 0; i < preset_count; i++)
	{
		if (model.getPreset(i).name == preset_name)
		{
			return setPreset(i, notify);
		}
	}

	return false;
}

bool FxController::setPreset(int selected_index, bool notify)
{
	auto& model = FxModel::getModel();

	if (selected_index < 0 || selected_index >= model.getPresetCount())
	{
		return false;
	}

	auto preset = model.getPreset(selected_index);

	auto current_preset = model.getSelectedPreset();
	// Auto-save the current preset if it was modified before switching away
	if (selected_index != current_preset)
	{
		if (model.isPresetModified(current_preset))
			autoSavePreset(current_preset);
	}

	if (preset.path.isNotEmpty())
	{
		// If this preset has an auto-saved version, load it instead of the original
		auto auto_save_path = getAutoSavePresetPath(preset.name);
		if (File(auto_save_path).existsAsFile())
		{
			dfx_dsp_.loadPreset(auto_save_path.toWideCharPointer());
			model.setPresetModified(selected_index, true);
		}
		else
		{
			dfx_dsp_.loadPreset(preset.path.toWideCharPointer());
			model.setPresetModified(selected_index, false);
		}

		settings_.setString("preset", preset.name);
		model.selectPreset(selected_index, true);

		for (auto e = 0; e < FxEffects::EffectType::NumEffects; e++)
		{
			auto value = dfx_dsp_.getEffectValue(static_cast<DfxDsp::Effect>(e));
			dfx_dsp_.setEffectValue(static_cast<DfxDsp::Effect>(e), value * 10);
		}

		auto num_bands = getNumEqBands();
		for (auto b = 0; b < num_bands; b++)
		{
			dfx_dsp_.setEqBandFrequency(b, dfx_dsp_.getEqBandFrequency(b));
			dfx_dsp_.setEqBandBoostCut(b, dfx_dsp_.getEqBandBoostCut(b));
		}
	}

	if (notify && FxModel::getModel().getPowerState())
	{
		model.pushMessage(TRANS("Preset: ") + model.getPreset(selected_index).name);
	}

	return true;
}

bool FxController::isPlaybackDeviceAvailable()
{
	return playback_device_available_;
}

void FxController::checkDeviceChanges()
{
	audio_passthru_->checkDeviceChanges();
}

void FxController::savePreset(const String& preset_name)
{
	ScopedLock lock(save_lock_);

	auto& model = FxModel::getModel();

	auto preset_index = model.getSelectedPreset();
	auto preset = model.getPreset(preset_index);

	if (preset_name.isEmpty())
	{
		auto path = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName()) + L"FxSound\\Presets";
		dfx_dsp_.savePreset(preset.name.toWideCharPointer(), path.toWideCharPointer());

		deleteAutoSavedPreset(preset.name);
		model.setPresetModified(preset_index, false);

		model.pushMessage(FormatString(TRANS(L"Changes to preset %s are saved."), preset.name));
	}
	else
	{
		auto path = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName()) + L"FxSound\\Presets";
		dfx_dsp_.savePreset(preset_name.toWideCharPointer(), path.toWideCharPointer());

		deleteAutoSavedPreset(preset.name);

		initPresets();

		setPreset(preset_name);

		model.pushMessage(FormatString(TRANS("New preset %s is saved."), preset_name));

		if (model.getUserPresetCount() == FxController::getInstance().getMaxUserPresets())
		{
			Thread::sleep(2000);
			model.pushMessage(TRANS("Reached the limit on new presets."));
		}
	}
}

void FxController::renamePreset(const String& new_name)
{
	ScopedLock lock(save_lock_);

	auto& model = FxModel::getModel();

	auto preset_index = model.getSelectedPreset();
	auto preset = model.getPreset(preset_index);

	if (preset.name == new_name) return;
	if (preset.type == FxModel::PresetType::UserPreset)
	{
		auto path = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName()) + L"FxSound\\Presets";
		dfx_dsp_.savePreset(new_name.toWideCharPointer(), path.toWideCharPointer());

		wchar_t old_path[MAX_PATH] = {};
		wcscpy_s(old_path, preset.path.toWideCharPointer());
		if (preset.path.length() + 1 < MAX_PATH)
		{
			old_path[preset.path.length() + 1] = L'\0';
		}
		SHFILEOPSTRUCT file_op = { NULL, FO_DELETE, old_path, L"",
								   FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT, FALSE, 0, L"" };
		SHFileOperation(&file_op);

		initPresets();

		setPreset(new_name);

		deleteAutoSavedPreset(preset.name);
		model.setPresetModified(FxModel::getModel().getSelectedPreset(), false);
	}
}

void FxController::deletePreset()
{
	auto& model = FxModel::getModel();

	auto preset_index = model.getSelectedPreset();
	auto preset = model.getPreset(preset_index);

	if (preset.type == FxModel::PresetType::UserPreset)
	{
		deleteAutoSavedPreset(preset.name);

		wchar_t path[MAX_PATH] = {};

		wcscpy_s(path, preset.path.toWideCharPointer());
		if (preset.path.length() + 1 < MAX_PATH)
		{
			path[preset.path.length() + 1] = L'\0';
		}

		SHFILEOPSTRUCT file_op = { NULL, FO_DELETE, path, L"",
								   FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT, FALSE, 0, L"" };
		SHFileOperation(&file_op);

		initPresets();

		auto device_config = DeviceConfig::getDeviceConfig(settings_, getOutputName());
		if (device_config.preset.isNotEmpty())
		{
			setPreset(device_config.preset, false);
		}
		else
		{
			setPreset(0);
		}

		FxModel::getModel().pushMessage(FormatString(TRANS("Preset %s is deleted."), preset.name));
	}
}

void FxController::undoPreset()
{
	auto& model = FxModel::getModel();

	auto preset_index = model.getSelectedPreset();
	if (!model.isPresetModified(preset_index))
		return;

	model.setPresetModified(preset_index, false);

	// Delete auto-save and clear modified flag before calling setPreset so it loads from original path
	auto preset = model.getPreset(preset_index);
	deleteAutoSavedPreset(preset.name);

	setPreset(preset_index);
}

void FxController::resetPresets()
{
	auto& model = FxModel::getModel();

	setNumEqBands(DEFAULT_NUM_EQ_BANDS);
	setVolumeLeveling(DEFAULT_VOLUME_LEVELING);
	setBalance(DEFAULT_BALANCE);
	setFilterQ(DEFAULT_FILTER_Q);
	setMasterGain(DEFAULT_MASTER_GAIN);

	auto count = model.getPresetCount();
	for (auto i = 0; i < count; i++)
	{
		auto preset = model.getPreset(i);
		if (preset.modified)
		{
			deleteAutoSavedPreset(preset.name);
		}

		if (preset.type == FxModel::PresetType::UserPreset)
		{
			wchar_t path[MAX_PATH] = {};

			wcscpy_s(path, preset.path.toWideCharPointer());
			if (preset.path.length() + 1 < MAX_PATH)
			{
				path[preset.path.length() + 1] = L'\0';
			}

			SHFILEOPSTRUCT file_op = { NULL, FO_DELETE, path, L"",
									   FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT, FALSE, 0, L"" };
			SHFileOperation(&file_op);
		}
	}

	initPresets();

	auto device_config = DeviceConfig::getDeviceConfig(settings_, getOutputName());
	if (device_config.preset.isNotEmpty())
	{
		setPreset(device_config.preset, false);
	}
	else
	{
		setPreset(0);
	}

	FxModel::getModel().pushMessage(TRANS("Presets are restored to factory defaults"));
}

void FxController::exportPresets(const Array< FxModel::Preset>& presets, std::function<void (bool)> onComplete)
{
	auto path_name = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userDocumentsDirectory).getFullPathName()) + L"FxSound\\Presets\\Export\\";

	File path(path_name);
	if (!path.exists())
	{
		path.createDirectory();
	}

	auto export_path = path.getFullPathName();

	// Walk the presets one at a time instead of a blocking for-loop: FxConfirmationMessage::showMessage()
	// is asynchronous now, so each preset only moves on to the next once the user has answered (or once
	// it is exported outright, when no overwrite confirmation is needed).
	auto presets_to_process = std::make_shared<Array<FxModel::Preset>>(presets);
	auto index = std::make_shared<int>(0);
	auto exported = std::make_shared<bool>(false);

	auto processNext = std::make_shared<std::function<void()>>();
	*processNext = [this, presets_to_process, index, exported, path_name, export_path, processNext, onComplete]()
	{
		if (*index >= presets_to_process->size())
		{
			onComplete(*exported);
			*processNext = nullptr;
			return;
		}

		auto preset = (*presets_to_process)[(*index)++];
		auto preset_file = File(path_name + preset.name + ".fac");

		auto doExport = [this, preset, export_path, exported, processNext]()
		{
			dfx_dsp_.exportPreset(preset.path.toWideCharPointer(), preset.name.toWideCharPointer(), export_path.toWideCharPointer());
			*exported = true;
			(*processNext)();
		};

		if (preset_file.exists())
		{
			FxConfirmationMessage::showMessage(FormatString(TRANS("Preset file %s already exists in the export path, do you want to overwrite the preset file?"), preset.name),
				FxConfirmationMessage::Style::YesNo,
				[doExport, processNext](bool overwrite)
				{
					if (overwrite)
					{
						doExport();
					}
					else
					{
						(*processNext)();
					}
				});
		}
		else
		{
			doExport();
		}
	};

	(*processNext)();
}

bool FxController::importPresets(const Array<File>& preset_files, StringArray& imported_presets, StringArray& skipped_presets)
{
	auto path_name = File::addTrailingSeparator(File::getSpecialLocation(File::SpecialLocationType::userApplicationDataDirectory).getFullPathName()) + L"FxSound\\Presets";
	File path(path_name);
	if (!path.exists())
	{
		path.createDirectory();
	}

	auto& model = FxModel::getModel();

	for (auto preset_file : preset_files)
	{
		auto preset_info = dfx_dsp_.getPresetInfo(preset_file.getFullPathName().toWideCharPointer());
		if (model.isPresetNameValid(preset_info.name.c_str()))
		{
			File import_file(path_name + "\\" + preset_info.name.c_str() + ".fac");
			if (preset_file.copyFileTo(import_file))
			{
				imported_presets.add(preset_info.name.c_str());
			}
		}
		else
		{
			skipped_presets.add(preset_info.name.c_str());
		}
	}

	if (!imported_presets.isEmpty())
	{
		initPresets();

		auto preset_name = settings_.getString("preset");
		setPreset(preset_name);

		return true;
	}

	return false;
}

void FxController::initOutputs(std::vector<SoundDevice>& sound_devices)
{
	ScopedLock auto_lock(lock_);

	dfx_enabled_ = false;

	auto device_configs_version = settings_.getInt("device_configs_version");
	if (device_configs_version != 2)
	{
		DeviceConfig::initDeviceConfigs(settings_, sound_devices);
		settings_.setInt("device_configs_version", 2);
	}
	else
	{
		auto device_configs = DeviceConfig::loadDeviceConfigs(settings_, "device_configs");
		if (device_configs.size() == 0)
		{
			// First time running the app, initialize device configs with sound devices
			DeviceConfig::initDeviceConfigs(settings_, sound_devices);
			settings_.setInt("device_configs_version", 2);
		}
		else
		{
			// Update device configs when relaunching the app
			DeviceConfig::updateDeviceConfigs(settings_, sound_devices);
		}
	}

	for (auto& sound_device : sound_devices)
	{
		if (!sound_device.isRealDevice && sound_device.deviceFriendlyName.find(L"FxSound Audio Enhancer") != std::wstring::npos)
		{
			dfx_enabled_ = true;
		}
	}

	refreshActiveOutputs(sound_devices);
	loadOutputSettings(sound_devices);

	// --output on the command line: enable only this output.
	if (startup_output_name_.isNotEmpty())
	{
		for (auto& device : active_output_devices_)
		{
			if (startup_output_name_ == device.deviceFriendlyName.c_str())
			{
				String id(device.pwszID.c_str());
				getOutputEntry(id);
				for (auto& entry : output_entries_)
				{
					entry.enabled = (entry.id == id);
				}
				saveOutputSettings();
				break;
			}
		}
		startup_output_name_.clear();
	}

	// The engine's carrier for now: the targeted device, or the Windows default device. powerOn() elects
	// the real one before processing starts.
	const SoundDevice* current = nullptr;
	for (auto& device : active_output_devices_)
	{
		if (device.isTargetedRealPlaybackDevice)
		{
			current = &device;
			break;
		}
		if (device.isDefaultDevice && current == nullptr)
		{
			current = &device;
		}
	}
	if (current != nullptr)
	{
		carrier_id_ = current->pwszID.c_str();
	}

	// Leaves the engine's legacy mode: from now on it plays exactly this set.
	pushOutputsToEngine();

	// The views are built before init(); this first notification fills them.
	notifyOutputsChanged();
}

// Handled when FxSound processing is on, after every reinit of the engine and when the HDMI wait ends.
void FxController::updateOutputs(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_names)
{
	auto prev_active_devices = active_output_devices_;

	const bool list_changed = refreshActiveOutputs(sound_devices);
	bool outputs_changed = list_changed;
	if (enableNewOutputs(new_device_names))
	{
		outputs_changed = true;
	}

	if (list_changed)
	{
		failed_carrier_ids_.clear();
	}

	// The carrier the engine is using after this reinit.
	String engine_carrier_id;
	for (auto& sound_device : sound_devices)
	{
		if (sound_device.isRealDevice && sound_device.isTargetedRealPlaybackDevice)
		{
			engine_carrier_id = sound_device.pwszID.c_str();
			break;
		}
	}

	if (requested_carrier_id_.isNotEmpty())
	{
		// A switch asked for with setAsPlaybackDevice(). If the engine did not take it although the device
		// is there, don't ask for it again until the devices change, so this can't loop.
		if (engine_carrier_id != requested_carrier_id_ && findActiveOutput(requested_carrier_id_) != nullptr)
		{
			logMessage("Output switch not taken by the audio engine: " + requested_carrier_id_);
			failed_carrier_ids_.addIfNotAlreadyThere(requested_carrier_id_);
		}
		requested_carrier_id_.clear();
	}
	else if (engine_carrier_id.isNotEmpty() && engine_carrier_id != carrier_id_ && !list_changed && !hdmi_reconnect_pending_)
	{
		// Same devices, another carrier: the user made this device the Windows default while FxSound was
		// processing. As before, that selects it: it is enabled (the other enabled outputs keep playing).
		if (findActiveOutput(engine_carrier_id) != nullptr)
		{
			auto& entry = getOutputEntry(engine_carrier_id);
			if (!entry.enabled)
			{
				entry.enabled = true;
				saveOutputSettings();
				outputs_changed = true;
			}
		}
	}

	// HDMI output handling around sleep. The HDMI removal is notified before the system suspend, so when
	// the enabled HDMI output that was the carrier disappears, the election is held (hdmi_reconnect_pending_)
	// until HDMI_FAILOVER_INTERVAL ticks pass:
	//  - HDMI reconnects in time (including after sleep): it is still enabled and is elected again.
	//  - Not in time: physical disconnect, a new carrier is elected among the enabled outputs.
	if (!hdmi_reconnect_pending_ && list_changed && carrier_id_.isNotEmpty() && findActiveOutput(carrier_id_) == nullptr)
	{
		auto* entry = findOutputEntry(carrier_id_);
		if (entry != nullptr && entry->enabled)
		{
			for (auto& prev_device : prev_active_devices)
			{
				if (prev_device.deviceFormFactor == L"HDMI" && carrier_id_ == prev_device.pwszID.c_str())
				{
					failover_output_id_ = carrier_id_;
					hdmi_failover_counter_ = 0;
					hdmi_reconnect_pending_ = true;
					break;
				}
			}
		}
	}

	if (hdmi_reconnect_pending_ &&
		(findActiveOutput(failover_output_id_) != nullptr || hdmi_failover_counter_ >= HDMI_FAILOVER_INTERVAL))
	{
		clearOutputRestore();
	}

	carrier_id_ = engine_carrier_id;

	// Waiting for the lost HDMI output to reconnect: stay muted and keep the outputs as they are.
	// timerCallback() ends the wait.
	if (hdmi_reconnect_pending_)
	{
		if (FxModel::getModel().getPowerState())
		{
			audio_passthru_->mute(true);
		}
		pushOutputsToEngine();
		if (outputs_changed)
		{
			notifyOutputsChanged();
		}
		return;
	}

	if (active_output_devices_.empty())
	{
		pushOutputsToEngine();
		if (outputs_changed)
		{
			notifyOutputsChanged();
		}
		handleNoOutput();
		return;
	}

	// Zero enabled outputs is fine here: the carrier plays silence and processing goes on.
	applyCarrier();

	if (outputs_changed)
	{
		notifyOutputsChanged();
	}

	if (FxModel::getModel().getPowerState() && !system_sleeping_)
	{
		audio_passthru_->mute(false);
	}
}

// Handled when FxSound processing is on
void FxController::selectProcessingOutput(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_names)
{
	updateOutputs(sound_devices, new_device_names);

	if (sound_devices.size() != device_count_)
	{
		device_count_ = (uint32_t)sound_devices.size();

		if (!dfx_enabled_)
		{
			stopTimer();
			if (!SysInfo::isRemoteSession())
			{
				main_window_->removeFromDesktop();
                auto* error_message = new FxDeviceErrorMessage();
                error_message->enterModalState(true, ModalCallbackFunction::create([](int)
                {
                    JUCEApplication::getInstance()->systemRequestedQuit();
                }), true);
				return;
			}
		}
	}

	// Sync after the outputs are updated, as the HDMI wait (if any) may have just ended.
	syncPlaybackAvailability();
}

// Notify the UI when the playback device availability changes (available <-> unavailable).
// Skipped while waiting for the HDMI output to reconnect, so no error is shown meanwhile; the
// state is synced again after the wait ends.
void FxController::syncPlaybackAvailability()
{
	if (hdmi_reconnect_pending_)
	{
		return;
	}

	auto available = audio_passthru_->isPlaybackDeviceAvailable();
	if (available != playback_device_available_)
	{
		playback_device_available_ = available;
		FxModel::getModel().notifyOutputError();
	}
}

// Handled when FxSound processing is off. The outputs are only shown and stored; the Windows default
// device is left alone. powerOn() elects the carrier when processing starts again.
void FxController::syncOutputsWhileOff(std::vector<SoundDevice>& sound_devices, const StringArray& new_device_names)
{
	bool changed = refreshActiveOutputs(sound_devices);
	if (enableNewOutputs(new_device_names))
	{
		changed = true;
	}

	device_count_ = (uint32_t)sound_devices.size();

	if (changed)
	{
		failed_carrier_ids_.clear();
		pushOutputsToEngine();
		notifyOutputsChanged();
	}

	// Processing was stopped because no output device was present: start again now that one is. This runs
	// on the device notification thread (IMMNotificationClient), so the restart goes to the message thread.
	if (no_output_ && !active_output_devices_.empty() && FxModel::getModel().getPowerState())
	{
		MessageManager::callAsync([this]() {
			bool restart;
			{
				ScopedLock auto_lock(lock_);
				restart = no_output_ && !active_output_devices_.empty() && !isTimerRunning();
			}
			if (restart && FxModel::getModel().getPowerState())
			{
				powerOn(true);
			}
		});
	}
}

// No real output device is present at all: nothing can play, stop processing until one comes back
// (syncOutputsWhileOff() starts it again). Zero *enabled* outputs does not come here.
void FxController::handleNoOutput()
{
	no_output_ = true;
	audio_passthru_->mute(true);
	powerOn(false);

	FxModel::getModel().pushMessage(TRANS("Output Disconnected"));
}

StringArray FxController::findNewDeviceNames(const std::vector<SoundDevice>& sound_devices)
{
	StringArray names;
	auto device_configs = DeviceConfig::loadDeviceConfigs(settings_, "device_configs");

	for (auto& sound_device : sound_devices)
	{
		if (!sound_device.isRealDevice)
			continue;

		String name(sound_device.deviceFriendlyName.c_str());
		bool known = false;
		for (auto& device_config : device_configs)
		{
			if (device_config.device_name == name)
			{
				known = true;
				break;
			}
		}
		if (!known)
		{
			names.addIfNotAlreadyThere(name);
		}
	}

	return names;
}

void FxController::sortByDeviceConfigPriority(std::vector<SoundDevice>& devices)
{
	auto device_configs = DeviceConfig::loadDeviceConfigs(settings_, "device_configs");

	std::stable_sort(devices.begin(), devices.end(),
		[&](const SoundDevice& a, const SoundDevice& b) {
			return compareOutputDevicePriority(
				String(a.deviceFriendlyName.c_str()),
				String(b.deviceFriendlyName.c_str()),
				device_configs) < 0;
		});
}

void FxController::powerOn(bool on)
{
	if (on)
	{
		dfx_dsp_.powerOn(true);

		if (!isTimerRunning())
		{
			// BetterFxSound outputs rework: elect the carrier before processing starts, so the first
			// reinit already uses it. While off, the engine takes the Windows default device when it starts.
			if (audio_passthru_ != nullptr)
			{
				ScopedLock auto_lock(lock_);

				auto sound_devices = audio_passthru_->getSoundDevices(true);
				if (refreshActiveOutputs(sound_devices))
				{
					notifyOutputsChanged();
				}

				requested_carrier_id_.clear();
				failed_carrier_ids_.clear();

				if (!active_output_devices_.empty())
				{
					applyCarrier();
				}
				else
				{
					pushOutputsToEngine();
				}

				if (no_output_)
				{
					no_output_ = false;
					audio_passthru_->mute(false);
				}
			}

			startTimer(100);
		}
	}
	else
	{
		dfx_dsp_.powerOn(false);

		{
			ScopedLock auto_lock(lock_);

			if (hdmi_reconnect_pending_)
			{
				audio_passthru_->mute(false);
				clearOutputRestore();
			}
		}

		if (isTimerRunning())
		{
			stopTimer();
		}

		audio_passthru_->restoreDefaultPlaybackDevice();
	}
}

float FxController::getEffectValue(FxEffects::EffectType effect)
{
	return dfx_dsp_.getEffectValue(static_cast<DfxDsp::Effect>(effect));
}

void FxController::setEffectValue(FxEffects::EffectType effect, float value)
{
	if (value < 0 || value > 10)
	{
		return;
    }

	dfx_dsp_.setEffectValue(static_cast<DfxDsp::Effect>(effect), value);

	auto& model = FxModel::getModel();
	if (!model.isPresetModified())
	{
		model.setPresetModified(model.getSelectedPreset(), true);
	}
	preset_dirty_ = true;
}

int FxController::getNumEqBands()
{
	return dfx_dsp_.getNumEqBands();
}

void FxController::setNumEqBands(int num_bands)
{
	dfx_dsp_.setNumBands(num_bands);
	settings_.setInt("num_bands", num_bands);
}

float FxController::getVolumeLeveling()
{
	return dfx_dsp_.getVolumeLeveling();
}

void FxController::setVolumeLeveling(float gain_db)
{
	gain_db = std::round(gain_db * 2.0f) / 2.0f;
	dfx_dsp_.setVolumeLeveling(gain_db);
	settings_.setDouble("volume_leveling", gain_db);
}

float FxController::getBalance()
{
	return dfx_dsp_.getBalance();
}

void FxController::setBalance(float balance_db)
{
	balance_db = std::round(balance_db);
	dfx_dsp_.setBalance(balance_db);
	settings_.setDouble("balance", balance_db);
}

float FxController::getMasterGain()
{
	return dfx_dsp_.getMasterGain();
}

void FxController::setMasterGain(float gain_db)
{
	gain_db = std::round(gain_db);
	dfx_dsp_.setMasterGain(gain_db);
	settings_.setDouble("master_gain", gain_db);
}

float FxController::getFilterQ()
{
	return dfx_dsp_.getFilterQ();
}

void FxController::setFilterQ(float q_multiplier)
{
	q_multiplier = std::round(q_multiplier * 2.0f) / 2.0f;
	dfx_dsp_.setFilterQ(q_multiplier);
	settings_.setDouble("filter_q", q_multiplier);
}

bool FxController::isAudioProcessing()
{
	return audio_process_on_;
}

float FxController::getEqBandFrequency(int band_num)
{
	if (band_num < getNumEqBands())
	{
		return dfx_dsp_.getEqBandFrequency(band_num);
	}
	else
	{
		return 0;
	}
}

void FxController::setEqBandFrequency(int band_num, float freq)
{
	if (band_num < getNumEqBands())
	{
        float min_freq, max_freq;
        getEqBandFrequencyRange(band_num, &min_freq, &max_freq);
		if (freq < min_freq || freq > max_freq)
		{
			return;
        }

		dfx_dsp_.setEqBandFrequency(band_num, freq);

		auto& model = FxModel::getModel();
		if (!model.isPresetModified())
		{
			model.setPresetModified(model.getSelectedPreset(), true);
		}
		preset_dirty_ = true;
	}
}

void FxController::getEqBandFrequencyRange(int band_num, float* min_freq, float* max_freq)
{
	if (band_num < getNumEqBands())
	{
		dfx_dsp_.getEqBandFrequencyRange(band_num, min_freq, max_freq);
	}
	else
	{
		*min_freq = 0;
		*max_freq = 0;
	}
}

bool FxController::getDefaultEqBandFrequency(int band_num, float* freq)
{
	if (band_num < getNumEqBands())
	{
		return dfx_dsp_.getDefaultEqBandFrequency(band_num, freq);
	}

	return false;
}

float FxController::getEqBandBoostCut(int band_num)
{
	if (band_num < getNumEqBands())
	{
		return dfx_dsp_.getEqBandBoostCut(band_num);
	}
	else
	{
		return 0;
	}
}

void FxController::setEqBandBoostCut(int band_num, float boost)
{
	if (band_num < getNumEqBands())
	{
		if (boost < MIN_GAIN || boost > MAX_GAIN)
		{
			return;
        }

		dfx_dsp_.setEqBandBoostCut(band_num, boost);

		auto& model = FxModel::getModel();
		if (!model.isPresetModified())
		{
			model.setPresetModified(model.getSelectedPreset(), true);
		}
		preset_dirty_ = true;
	}
}

LRESULT CALLBACK FxController::eventCallback(HWND hwnd, const UINT message, const WPARAM w_param, const LPARAM l_param)
{
	static auto os = SystemStats::getOperatingSystemType();
	FxController* controller = (FxController*)GetWindowLongPtr(hwnd, GWLP_USERDATA);

	switch (message)
	{
	case WM_HOTKEY:
	{
		if (w_param == CMD_ON_OFF)
		{
			if (!SysInfo::isRemoteSession())
			{
				auto power_state = !FxModel::getModel().getPowerState();
				controller->setPowerState(power_state);

				String param = FxModel::getModel().getPowerState() ? TRANS(L"on") : TRANS(L"off");
				FxModel::getModel().pushMessage(controller->FormatString(TRANS("FxSound is %s."), param));
			}
		}
		if (w_param == CMD_OPEN_CLOSE)
		{
			if (controller->main_window_->isOnDesktop())
			{
				controller->hideMainWindow();
			}
			else
			{
				controller->showMainWindow();
			}
		}
		if (w_param == CMD_NEXT_PRESET && FxModel::getModel().getPowerState())
		{
			auto preset_index = FxModel::getModel().getSelectedPreset();
			auto preset_count = FxModel::getModel().getPresetCount();
			if (preset_count > 1)
			{
				if (preset_index < preset_count - 1)
				{
					preset_index++;
				}
				else
				{
					preset_index = 0;
				}

				controller->setPreset(preset_index);
			}
		}
		if (w_param == CMD_PREVIOUS_PRESET && FxModel::getModel().getPowerState())
		{
			auto preset_index = FxModel::getModel().getSelectedPreset();
			auto preset_count = FxModel::getModel().getPresetCount();
			if (preset_count > 1)
			{
				if (preset_index != 0)
				{
					preset_index--;
				}
				else
				{
					preset_index = preset_count - 1;
				}

				controller->setPreset(preset_index);
			}
		}
		if (w_param == CMD_NEXT_OUTPUT)
		{
			// BetterFxSound outputs rework: enable only the next output, cycling.
			controller->enableNextOutput();
		}
	}
	break;

	case WM_POWERBROADCAST:
	{
		if (w_param == PBT_APMSUSPEND)
		{
			controller->onSystemSuspend();
		}
		else if (w_param == PBT_APMRESUMESUSPEND || w_param == PBT_APMRESUMEAUTOMATIC)
		{
			controller->onSystemResume();
		}
	}
	break;

	case WM_WTSSESSION_CHANGE:
	{
		WPARAM login_event = (os == SystemStats::OperatingSystemType::Windows7) ? WTS_CONSOLE_CONNECT : WTS_SESSION_DESKTOP_READY;

		if (w_param == login_event || w_param == WTS_SESSION_UNLOCK)
		{
			controller->setPowerState(controller->settings_.getBool("power"));
		}
		else if (w_param == WTS_CONSOLE_DISCONNECT)
		{
			controller->powerOn(false);
		}
		else if (w_param == WTS_REMOTE_CONNECT)
		{
			controller->setPowerState(controller->settings_.getBool("power"));
		}
	}
	}

	return DefWindowProc(hwnd, message, w_param, l_param);
}

void FxController::timerCallback()
{
	if (output_changed_)
	{
		output_changed_ = false;
		Thread::sleep(200);
		return;
	}

	{
		// No suspend followed the HDMI removal (physical disconnect), or HDMI did not return within
		// the wait after wake: continue on the available output as per preference.
		ScopedLock auto_lock(lock_);
		// The timer keeps running for a few seconds after the suspend notification, before the system
		// actually sleeps. Don't count that time; the wait starts again on resume.
		if (!system_sleeping_ && (hdmi_reconnect_pending_ && ++hdmi_failover_counter_ >= HDMI_FAILOVER_INTERVAL))
		{
			// updateOutputs() ends the wait, elects a new carrier among the enabled outputs and unmutes.
			auto sound_devices = audio_passthru_->getSoundDevices(true);
			updateOutputs(sound_devices, StringArray());
		}
	}

	audio_passthru_->processTimer();
	{
		// Sample after processTimer(), once any device reinit has set the availability. This also covers
		// the HDMI wait ending through the timer, where selectProcessingOutput() may not be called.
		ScopedLock auto_lock(lock_);
		syncPlaybackAvailability();
	}

	auto total_audio_process_time = dfx_dsp_.getTotalAudioProcessedTime();
	if (audio_process_time_ != total_audio_process_time)
	{
		audio_process_time_ = total_audio_process_time;
		audio_process_on_counter_++;
		audio_process_off_counter_ = 0;
	}
	else
	{
		audio_process_off_counter_++;
		audio_process_on_counter_ = 0;
	}

	auto power = FxModel::getModel().getPowerState();
	if (audio_process_on_counter_ == 5 && !audio_process_on_)
	{
		audio_process_on_ = true;
		system_tray_view_->setStatus(power, true);
		main_window_->setIcon(power, true);
		main_window_->startLogoAnimation();
		if (view_ == ViewType::Pro)
		{
			main_window_->showProView();
			main_window_->startVisualizer();
		}
	}
	if (audio_process_off_counter_ == 5 && audio_process_on_)
	{
		audio_process_on_ = false;
		system_tray_view_->setStatus(power, false);
		main_window_->setIcon(power, false);
		main_window_->stopLogoAnimation();
		if (view_ == ViewType::Pro)
		{
			main_window_->showProView();
			main_window_->pauseVisualizer();
		}
	}

	// Auto-save modified preset once per minute if there are unsaved changes
	static constexpr int AUTO_SAVE_INTERVAL = 600; // 600 ticks * 100ms = 60 seconds
	if (++auto_save_counter_ >= AUTO_SAVE_INTERVAL)
	{
		if (preset_dirty_)
		{
			autoSavePreset(FxModel::getModel().getSelectedPreset());
		}
		auto_save_counter_ = 0;
	}

	auto current_time = Time::getCurrentTime();
	if (auto_updates_ && current_time.getHours() == 10 && current_time.getMinutes() == 00 && current_time.getSeconds() == 0)
	{
		checkUpdates();
	}
}

void FxController::onSoundDeviceChange(bool processing)
{
	if (session_id_ != WTSGetActiveConsoleSessionId())
		return; // Ignore device changes on another user session

	ScopedLock auto_lock(lock_);

	if (isTimerRunning())
	{
		if (processing)
		{
			output_devices_ = audio_passthru_->getSoundDevices(false);
			// Before updateDeviceConfigs() learns them: devices never seen before.
			auto new_device_names = findNewDeviceNames(output_devices_);
			DeviceConfig::updateDeviceConfigs(settings_, output_devices_);
			auto sound_devices = audio_passthru_->getSoundDevices(true);
			selectProcessingOutput(sound_devices, new_device_names);
		}
	}
	else
	{
		output_devices_ = audio_passthru_->getSoundDevices(false);
		auto new_device_names = findNewDeviceNames(output_devices_);
		DeviceConfig::updateDeviceConfigs(settings_, output_devices_);
		auto sound_devices = audio_passthru_->getSoundDevices(true);
		syncOutputsWhileOff(sound_devices, new_device_names);
	}
}

void FxController::onSystemSuspend()
{
	{
		ScopedLock auto_lock(lock_);

		hdmi_failover_counter_ = 0;

		system_sleeping_ = true;
	}

	if (FxModel::getModel().getPowerState())
	{
		audio_passthru_->mute(true);
	}
}

void FxController::onSystemResume()
{
	ScopedLock auto_lock(lock_);

	hdmi_failover_counter_ = 0;

	system_sleeping_ = false;

	if (!hdmi_reconnect_pending_ && FxModel::getModel().getPowerState())
	{
		audio_passthru_->mute(false);
	}
}

void FxController::clearOutputRestore()
{
	failover_output_id_ = String();
	hdmi_failover_counter_ = 0;
	hdmi_reconnect_pending_ = false;
}

void FxController::enableHotkeys(bool enable)
{
	settings_.setBool("hotkeys", enable);
	FxModel::getModel().setHotkeySupport(enable);
	if (enable)
	{
		registerHotkeys();
	}
	else
	{
		unregisterHotkeys();
	}
}

bool FxController::getHotkey(String cmdKey, int& mod, int& vk)
{
	int cmd = settings_.getInt(cmdKey);
	mod = (cmd >> 16) & 0x7;
	vk = cmd & 0xff;
	if ((mod == (MOD_CONTROL | MOD_ALT) || mod == (MOD_CONTROL | MOD_SHIFT)) && ((vk >= 0x30 && vk <= 0x39) || (vk >= 'A' && vk <= 'Z')))
	{
		return true;
	}

	mod = 0;
	vk = 0;

	return false;
}

bool FxController::setHotkey(const String& command, int new_mod, int new_vk)
{
	StringArray hotkey_cmds = { HK_CMD_ON_OFF, HK_CMD_OPEN_CLOSE, HK_CMD_NEXT_PRESET, HK_CMD_PREVIOUS_PRESET, HK_CMD_NEXT_OUTPUT };

	for (int i = 0; i < hotkey_cmds.size(); i++)
	{
		if (command == hotkey_cmds[i])
		{
			continue;
		}

		int mod, vk;
		if (getHotkey(hotkey_cmds[i], mod, vk))
		{
			if (mod == new_mod && vk == new_vk)
			{
				return false;
			}
		}
	}

	unsigned int code = (new_mod << 16) | new_vk;
	if (!isValidHotkey(new_mod, new_vk))
	{
		code = 0;
	}

	int hk_id = 0;

	if (command == HK_CMD_ON_OFF)
	{
		hk_id = CMD_ON_OFF;
	}

	if (command == HK_CMD_OPEN_CLOSE)
	{
		hk_id = CMD_OPEN_CLOSE;
	}

	if (command == HK_CMD_NEXT_PRESET)
	{
		hk_id = CMD_NEXT_PRESET;
	}

	if (command == HK_CMD_PREVIOUS_PRESET)
	{
		hk_id = CMD_PREVIOUS_PRESET;
	}

	if (command == HK_CMD_NEXT_OUTPUT)
	{
		hk_id = CMD_NEXT_OUTPUT;
	}

	if (hk_id != 0)
	{
		::UnregisterHotKey(message_window_.getHandle(), hk_id);

		if (code == 0)
		{
			settings_.setInt(command, code);
			return false;
		}

		if (::RegisterHotKey(message_window_.getHandle(), hk_id, new_mod, new_vk) != FALSE)
		{
			settings_.setInt(command, code);
			return true;
		}
		else
		{
			settings_.setInt(command, 0);
			return false;
		}
	}

	return false;
}

bool FxController::isValidHotkey(int mod, int vk)
{
	if ((mod & MOD_CONTROL) == 0)
	{
		return false;
	}

	HKL hkl = GetKeyboardLayout(0);
	BYTE kbd_state[256] = { 0 };
	kbd_state[VK_CONTROL] = 0x80;

	if ((mod & MOD_ALT) != 0)
	{
		kbd_state[VK_MENU] = 0x80;
	}
	if ((mod & MOD_SHIFT) != 0)
	{
		kbd_state[VK_SHIFT] = 0x80;
	}

	WCHAR output[3] = { 0 };

	int len = ToUnicodeEx(vk, 0, kbd_state, output, 3, 0, hkl);
	if (len > 0 && output[0] >= 0x20) // The hotkey combination generates a non-control character
	{
		return false;
	}

	return true;
}

bool FxController::isHelpTooltipsHidden()
{
	return hide_help_tooltips_;
}

void FxController::setHelpTooltipsHidden(bool status)
{
	hide_help_tooltips_ = status;
	settings_.setBool("hide_help_tooltips", status);
	main_window_->repaint();
}

bool FxController::isNotificationsHidden()
{
	return hide_notifications_;
}

void FxController::setNotificationsHidden(bool status)
{
	hide_notifications_ = status;
	settings_.setBool("hide_notifications", status);
}

String FxController::getLanguage() const
{
	return language_;
}

void FxController::setLanguage(String language_code)
{
	if (language_code.isEmpty())
	{
		language_code = "en";
	}

	language_ = language_code;
	settings_.setString("language", language_);

	LocalisedStrings::setCurrentMappings(nullptr);

	auto& language_info = FxLanguage::find(language_);
	if (language_info.translation_data != nullptr && language_info.translation_data_size != 0)
	{
		LocalisedStrings::setCurrentMappings(new LocalisedStrings(String::createStringFromData(language_info.translation_data, language_info.translation_data_size), false));
	}

	auto* theme = dynamic_cast<FxTheme*>(&LookAndFeel::getDefaultLookAndFeel());
	if (theme != nullptr)
	{
		theme->loadFont(language_);
	}

	// Other top-level windows (e.g. the Settings dialog) aren't children of
	// main_window_, so they don't hear about a language change unless we
	// tell them directly. Scoped to FxWindow-derived windows (dialogs) only,
	// so transient overlays like notifications and the tray icon view -
	// which don't show translated/language-dependent text - aren't
	// needlessly repainted on every language switch.
	auto& desktop = Desktop::getInstance();
	for (int i = 0; i < desktop.getNumComponents(); i++)
	{
		if (auto* window = dynamic_cast<FxWindow*>(desktop.getComponent(i)))
		{
			window->sendLookAndFeelChange();
		}
	}
}

int FxController::getMaxUserPresets() const
{
	return max_user_presets_;
}

bool FxController::getAutoUpdates()
{
	return auto_updates_;
}

void FxController::setAutoUpdates(bool enable)
{
	auto_updates_ = enable;
	settings_.setBool("automatic_updates", enable);
}

void FxController::checkUpdates()
{
	// BetterFxSound does not ship the official FxSound updater (updater.exe), which would
	// "update" to official FxSound. New versions are published on GitHub Releases instead.
}

void FxController::saveWindowPosition(int x, int y)
{
	settings_.setInt("window_x", x);
	settings_.setInt("window_y", y);
}

void FxController::getWindowPosition(int& x, int& y)
{
	x = settings_.getInt("window_x", 0);
	y = settings_.getInt("window_y", 0);
}

void FxController::setRenderingEngine(juce::Component& component)
{
	if (auto* peer = component.getPeer())
	{
		// Engine index 0 is GDI (software), 1 is Direct2D.
		peer->setCurrentRenderingEngine(gpu_render_ ? 1 : 0);
	}
}

bool FxController::isGpuRenderingEnabled()
{
	return settings_.getBool("gpu_render");
}

void FxController::setGpuRenderingEnabled(bool enable)
{
	settings_.setBool("gpu_render", enable);
}

bool FxController::isGpuRenderingActive() const
{
	return gpu_render_;
}

bool FxController::canSupportGpuRendering()
{
#if JUCE_MAJOR_VERSION >= 8
	return juce::SystemStats::getOperatingSystemType() > juce::SystemStats::Windows7;
#else
	return false;
#endif
}

bool FxController::launchNewInstance()
{
	auto& controller = getInstance();

	wchar_t module_file[MAX_PATH];
	DWORD length = GetModuleFileNameW(NULL, module_file, MAX_PATH);
	if (length == 0 || length >= MAX_PATH)
	{
		controller.logMessage("Relaunch failed: module path unavailable\n");
		return false;
	}

	// A hidden helper waits ~3 seconds before starting the exe, because the single instance
	// check would otherwise see this process while it is still exiting.
	std::wstring command_line = L"cmd.exe /c \"ping -n 4 127.0.0.1 >nul & start \"\" \"";
	command_line += module_file;
	command_line += L"\"\"";

	// CREATE_NO_WINDOW gives cmd a hidden console that ping shares. It must not be combined
	// with DETACHED_PROCESS: Windows then ignores CREATE_NO_WINDOW, and ping, having no console
	// to inherit, opens a visible one of its own.
	STARTUPINFOW startup_info = { sizeof(STARTUPINFOW) };
	startup_info.dwFlags = STARTF_USESHOWWINDOW;
	startup_info.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION process_info = { 0 };
	if (CreateProcessW(NULL, command_line.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup_info, &process_info))
	{
		CloseHandle(process_info.hThread);
		CloseHandle(process_info.hProcess);
		controller.logMessage("Relaunching\n");
		return true;
	}

	controller.logMessage("Relaunch failed\n");
	return false;
}

void FxController::restartApplication()
{
	autoSaveModifiedPreset();

	if (launchNewInstance())
	{
		// Close open modal dialogs (e.g. Settings) first; their deletion is queued before the
		// quit below, so they are gone before shutdown() tears down the windows.
		if (auto* modal_manager = ModalComponentManager::getInstanceWithoutCreating())
		{
			modal_manager->cancelAllModalComponents();
		}

		MessageManager::callAsync([]()
		{
			JUCEApplication::getInstance()->systemRequestedQuit();
		});
	}
}

juce::Array<DeviceConfig> FxController::getDeviceConfigs()
{
	return DeviceConfig::loadDeviceConfigs(settings_, "device_configs");
}

void FxController::saveDeviceConfigs(const juce::Array<DeviceConfig>& device_configs)
{
	DeviceConfig::saveDeviceConfigs(settings_, "device_configs", device_configs);
}

bool FxController::isOutputDeviceConnected(const String& output_device_name)
{
	ScopedLock auto_lock(lock_);

	for (auto& output_device : active_output_devices_)
	{
		if (output_device_name == output_device.deviceFriendlyName.c_str())
		{
			return true;
		}
	}

	return false;
}

bool FxController::isOutputDevicePresent(const String& output_device_name)
{
	for (auto& output_device : output_devices_)
	{
		if (output_device.isRealDevice && output_device_name == output_device.deviceFriendlyName.c_str())
		{
			return true;
		}
	}

	return false;
}

SoundDevice FxController::getPreferredOutput()
{
	auto device_configs = DeviceConfig::loadDeviceConfigs(settings_, "device_configs");

	for (auto& device_config : device_configs)
	{
		for (auto& device : active_output_devices_)
		{
			if (device_config.device_name == device.deviceFriendlyName.c_str())
			{
				return device;
			}
		}
	}

	if (active_output_devices_.size() > 0)
	{
		return active_output_devices_[0];
	}

	return {};
}

int FxController::compareOutputDevicePriority(const String& output_device_name1, const String& output_device_name2, const juce::Array<DeviceConfig>& device_configs)
{
	int priority1 = (int)device_configs.size();
	int priority2 = (int)device_configs.size();
	int index = 0;

	for (auto& device_config : device_configs)
	{
		if (device_config.device_name == output_device_name1)
			priority1 = index;
		if (device_config.device_name == output_device_name2)
			priority2 = index;

		if (priority1 != (int)device_configs.size() && priority2 != (int)device_configs.size())
			break;

		index++;
	}

	return priority1 - priority2;
}

void FxController::refreshOutputList()
{
	// The device priority may have changed in Output preferences: elect the carrier again.
	ScopedLock auto_lock(lock_);

	if (isTimerRunning() && !hdmi_reconnect_pending_ && !active_output_devices_.empty())
	{
		applyCarrier();
	}
	notifyOutputsChanged();
}

const String& FxController::getOutputName()
{
	ScopedLock auto_lock(lock_);

	output_device_name_ = String();
	if (auto* entry = findOutputEntry(getSoleEnabledOutputId()))
	{
		auto* device = findActiveOutput(entry->id);
		output_device_name_ = device != nullptr ? String(device->deviceFriendlyName.c_str()) : entry->name;
	}
	return output_device_name_;
}

bool FxController::isNewOutputPrioritized()
{
	return settings_.getBool("prioritize_new_output", false);
}

void FxController::setNewOutputPrioritized(bool prioritize)
{
	settings_.setBool("prioritize_new_output", prioritize);
}

bool FxController::startDelayCalibration()
{
	return audio_passthru_ != nullptr && audio_passthru_->startDelayCalibration();
}

void FxController::cancelDelayCalibration()
{
	if (audio_passthru_ != nullptr)
		audio_passthru_->cancelDelayCalibration();
}

int FxController::getDelayCalibrationState(int* progress_percent)
{
	if (audio_passthru_ == nullptr)
		return 0;
	return audio_passthru_->getDelayCalibrationState(progress_percent);
}

String FxController::getDelayCalibrationError()
{
	if (audio_passthru_ == nullptr)
		return {};
	return String(audio_passthru_->getDelayCalibrationError().c_str());
}

String FxController::applyDelayCalibrationResults()
{
	if (audio_passthru_ == nullptr)
		return {};

	// One result per measured output, keyed by device_id, in no particular order. There is no main output:
	// the measured outputs are aligned to the slowest one.
	auto results = audio_passthru_->getDelayCalibrationResults();
	if (results.empty())
		return TRANS("Nothing was measured.");

	ScopedLock auto_lock(lock_);

	auto nameOf = [this](const String& id) -> String {
		if (auto* device = findActiveOutput(id))
			return String(device->deviceFriendlyName.c_str());
		if (auto* entry = findOutputEntry(id))
		{
			if (entry->name.isNotEmpty())
				return entry->name;
		}
		return id;
	};

	auto currentDelay = [this](const String& id) {
		auto* entry = findOutputEntry(id);
		return entry != nullptr ? entry->delay_ms : 0;
	};

	// Each measurement includes the delay the output already had. Remove it to get the output's own
	// latency, then delay every measured output so they all match the slowest one.
	bool any_measured = false;
	double slowest = 0.0;
	for (auto& result : results)
	{
		if (!result.measured)
			continue;

		auto own_latency = result.latency_ms - currentDelay(String(result.device_id.c_str()));
		slowest = any_measured ? jmax(slowest, own_latency) : own_latency;
		any_measured = true;
	}

	if (!any_measured)
		return TRANS("No output could be heard clearly, so no delay was changed. Turn the volume up, move the microphone closer and try again.");

	StringArray lines;
	StringArray not_heard;
	bool clamped = false;

	for (auto& result : results)
	{
		String id(result.device_id.c_str());
		auto name = nameOf(id);

		if (!result.measured)
		{
			// Keeps its delay.
			not_heard.add(name);
			continue;
		}

		auto own_latency = result.latency_ms - currentDelay(id);
		auto delay = roundToInt(slowest - own_latency);
		if (delay > 500)
		{
			delay = 500;
			clamped = true;
		}
		delay = jmax(0, delay);

		getOutputEntry(id).delay_ms = delay;

		lines.add(name + ": " + String(delay) + " ms");
	}

	saveOutputSettings();
	pushOutputsToEngine();
	notifyOutputsChanged();

	String summary = TRANS("Delays set:") + " " + lines.joinIntoString(", ") + ".";
	if (not_heard.size() > 0)
		summary += " " + TRANS("Not heard clearly, left unchanged:") + " " + not_heard.joinIntoString(", ") + ".";
	if (clamped)
		summary += " " + TRANS("Some devices differ by more than 500 ms, the maximum delay.");
	return summary;
}

FxThemeMode FxController::getThemeMode()
{
	return FxTheme::getThemeMode();
}

void FxController::setThemeMode(FxThemeMode mode)
{
	if (mode == getThemeMode())
	{
		return;
	}

	FxTheme::setThemeMode(mode);
	settings_.setInt("theme_mode", static_cast<int>(mode));
	setLanguage(getLanguage()); // To reload font for the new theme
	main_window_->sendLookAndFeelChange();

	auto power = FxModel::getModel().getPowerState();
	main_window_->setIcon(power, audio_process_on_);
	system_tray_view_->setStatus(power, audio_process_on_);
}

bool FxController::isAlwaysOnTop()
{
	return always_on_top_;
}

void FxController::setAlwaysOnTop(bool always_on_top)
{
	always_on_top_ = always_on_top;
	settings_.setBool("always_on_top", always_on_top);
	main_window_->setAlwaysOnTop(always_on_top);
}

bool FxController::isLaunchOnStartup()
{
	HKEY hkey;
	RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hkey);
	DWORD type;
	DWORD size = 0;
	RegQueryValueEx(hkey, L"FxSound", NULL, &type, NULL, &size);
	RegCloseKey(hkey);

	return size > 0;
}

void FxController::setLaunchOnStartup(bool launch_on_startup)
{
	wchar_t szPath[MAX_PATH];
	GetModuleFileName(NULL, szPath, MAX_PATH);
	HKEY hkey;
	RegOpenKeyEx(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hkey);

	if (launch_on_startup)
	{
		RegSetValueEx(hkey, L"FxSound", 0, REG_SZ, (BYTE*)szPath, sizeof(szPath));
	}
	else
	{
		RegDeleteValue(hkey, L"FxSound");
	}

	RegCloseKey(hkey);
}

void FxController::registerHotkeys()
{
	if (!hotkeys_registered_)
	{
		int mod;
		int vk;

		if (getHotkey(HK_CMD_ON_OFF, mod, vk))
		{
			if (isValidHotkey(mod, vk))
			{
				::RegisterHotKey(message_window_.getHandle(), CMD_ON_OFF, mod, vk);
			}
		}

		if (getHotkey(HK_CMD_OPEN_CLOSE, mod, vk))
		{
			if (isValidHotkey(mod, vk))
			{
				::RegisterHotKey(message_window_.getHandle(), CMD_OPEN_CLOSE, mod, vk);
			}

		}

		if (getHotkey(HK_CMD_NEXT_PRESET, mod, vk))
		{
			if (isValidHotkey(mod, vk))
			{
				::RegisterHotKey(message_window_.getHandle(), CMD_NEXT_PRESET, mod, vk);
			}

		}

		if (getHotkey(HK_CMD_PREVIOUS_PRESET, mod, vk))
		{
			if (isValidHotkey(mod, vk))
			{
				::RegisterHotKey(message_window_.getHandle(), CMD_PREVIOUS_PRESET, mod, vk);
			}

		}

		if (getHotkey(HK_CMD_NEXT_OUTPUT, mod, vk))
		{
			if (isValidHotkey(mod, vk))
			{
				::RegisterHotKey(message_window_.getHandle(), CMD_NEXT_OUTPUT, mod, vk);
			}
		}

		hotkeys_registered_ = true;
	}
}

void FxController::unregisterHotkeys()
{
	if (hotkeys_registered_)
	{
		::UnregisterHotKey(message_window_.getHandle(), CMD_ON_OFF);
		::UnregisterHotKey(message_window_.getHandle(), CMD_OPEN_CLOSE);
		::UnregisterHotKey(message_window_.getHandle(), CMD_NEXT_PRESET);
		::UnregisterHotKey(message_window_.getHandle(), CMD_PREVIOUS_PRESET);
		::UnregisterHotKey(message_window_.getHandle(), CMD_NEXT_OUTPUT);
		hotkeys_registered_ = false;
	}
}

void FxController::getSpectrumBandValues(Array<float>& band_values)
{
	float values[NUM_SPECTRUM_BANDS] = { 0 };

	dfx_dsp_.getSpectrumBandValues(values, NUM_SPECTRUM_BANDS);

	band_values.clearQuick();
	for (auto i = 0; i < NUM_SPECTRUM_BANDS; i++)
	{
		if (audio_process_on_)
		{
			band_values.set(i, values[i]);
		}
		else
		{
			band_values.set(i, 0.01f);
		}
	}
}

String FxController::FormatString(const String& format, const String& arg)
{
	wchar_t buffer[1024];

	swprintf_s(buffer, format.toWideCharPointer(), arg.toWideCharPointer());

	return String(buffer);
}

// ---------------------------------------------------------------------------------------------
// BetterFxSound outputs rework (docs/OUTPUTS_REWORK.md).
//
// Every output the user enabled or configured is an OutputEntry, saved as the "outputs" settings
// key. The engine always gets the whole set through AudioPassthru::setOutputs(). There is no main
// output: internally one present output is the carrier, the device on the original low latency
// path, switched with setAsPlaybackDevice(). The carrier is the enabled, present output that comes
// first in the device priority (Output preferences), so a given set of outputs always gets the same
// carrier and a delay calibration stays valid after a device leaves and comes back. With no enabled
// output present, the current carrier (or the preferred device) stays and plays silence.
// ---------------------------------------------------------------------------------------------

static OutputChannelMode toOutputChannelMode(int value)
{
	return static_cast<OutputChannelMode>(jlimit(0, 3, value));
}

FxController::OutputEntry* FxController::findOutputEntry(const String& id)
{
	if (id.isEmpty())
		return nullptr;

	for (auto& entry : output_entries_)
	{
		if (entry.id == id)
			return &entry;
	}
	return nullptr;
}

// The entry for this id, added (disabled, default settings) if there is none. The reference is only
// valid until the next entry is added.
FxController::OutputEntry& FxController::getOutputEntry(const String& id)
{
	if (auto* existing = findOutputEntry(id))
		return *existing;

	OutputEntry entry;
	entry.id = id;
	if (auto* device = findActiveOutput(id))
		entry.name = device->deviceFriendlyName.c_str();
	output_entries_.push_back(entry);
	return output_entries_.back();
}

const SoundDevice* FxController::findActiveOutput(const String& id) const
{
	if (id.isEmpty())
		return nullptr;

	for (auto& device : active_output_devices_)
	{
		if (id == device.pwszID.c_str())
			return &device;
	}
	return nullptr;
}

// Reads the "outputs" key, or migrates the old main output + additional outputs keys once, or on a first
// run enables the Windows default device. The old keys are removed afterwards.
void FxController::loadOutputSettings(const std::vector<SoundDevice>& sound_devices)
{
	static const char* const legacy_keys[] = { "output_device_name", "main_output_delay_ms", "main_output_channel_mode", "additional_outputs" };

	output_entries_.clear();

	auto addEntry = [this](const OutputEntry& entry) {
		if (entry.id.isNotEmpty() && findOutputEntry(entry.id) == nullptr)
			output_entries_.push_back(entry);
	};

	auto readEntry = [](const var& item, bool enabled_by_default) {
		OutputEntry entry;
		entry.id = item.getProperty("id", "").toString();
		entry.name = item.getProperty("name", "").toString();
		entry.enabled = static_cast<bool>(item.getProperty("enabled", enabled_by_default));
		entry.delay_ms = jlimit(0, 500, static_cast<int>(item.getProperty("delay_ms", 0)));
		entry.volume_percent = jlimit(0, 100, static_cast<int>(item.getProperty("volume", 100)));
		entry.channel_mode = toOutputChannelMode(static_cast<int>(item.getProperty("channel_mode", 0)));
		return entry;
	};

	auto realDeviceName = [&sound_devices](const String& id) -> String {
		for (auto& device : sound_devices)
		{
			if (device.isRealDevice && id == device.pwszID.c_str())
				return String(device.deviceFriendlyName.c_str());
		}
		return {};
	};

	bool has_legacy = false;
	for (auto* key : legacy_keys)
	{
		if (settings_.containsKey(key))
			has_legacy = true;
	}

	bool save = false;
	bool main_pending = false;	// The old main output could not be resolved: keep its keys for a later run
	const bool has_outputs = settings_.containsKey("outputs");

	if (has_outputs)
	{
		auto json = settings_.getJson("outputs");
		if (auto* array = json.getArray())
		{
			for (auto& item : *array)
				addEntry(readEntry(item, false));
		}
	}

	if (has_legacy)
	{
		// One-time migration. The old main output becomes an enabled output with its delay and channel
		// mode; the old additional outputs (the list only held enabled ones) keep their settings. If the main
		// output can't be resolved to a device id, its keys stay and every start tries again (with "outputs"
		// already there, only the main output is added, and only if that device has no entry yet).
		auto main_name = settings_.getString("output_device_name");
		if (main_name.isNotEmpty())
		{
			String main_id;
			for (int pass = 0; pass < 2 && main_id.isEmpty(); pass++)
			{
				for (auto& device : sound_devices)
				{
					// Active devices first, as several endpoints can share a name.
					if (device.isRealDevice && (pass == 1 || device.isActive) && main_name == device.deviceFriendlyName.c_str())
					{
						main_id = device.pwszID.c_str();
						break;
					}
				}
			}
			if (main_id.isEmpty())
				main_id = DeviceConfig::getDeviceConfig(settings_, main_name).device_id;

			if (main_id.isNotEmpty())
			{
				if (has_outputs && findOutputEntry(main_id) == nullptr)
					logMessage("Outputs migration: output \"" + main_name + "\" resolved on a later start, added");

				OutputEntry entry;
				entry.id = main_id;
				entry.name = main_name;
				entry.enabled = true;
				entry.delay_ms = jlimit(0, 500, settings_.getInt("main_output_delay_ms", 0));
				entry.channel_mode = toOutputChannelMode(settings_.getInt("main_output_channel_mode", 0));
				addEntry(entry);
				save = true;
			}
			else
			{
				main_pending = true;
				logMessage("Outputs migration: output \"" + main_name + "\" not found, its settings are kept to try again on the next start");
			}
		}

		if (!has_outputs)
		{
			auto json = settings_.getJson("additional_outputs");
			if (auto* array = json.getArray())
			{
				for (auto& item : *array)
				{
					auto entry = readEntry(item, true);
					entry.enabled = true;
					entry.name = realDeviceName(entry.id);
					addEntry(entry);
				}
			}

			save = true;
			logMessage("Outputs migrated from the main/additional output settings: " + String((int)output_entries_.size()) + " output(s)");

			// Kept as migrated even if none of them is connected: the outputs stay silent and the UI shows its notice.
			bool any_connected = false;
			for (auto& entry : output_entries_)
			{
				if (entry.enabled && findActiveOutput(entry.id) != nullptr)
					any_connected = true;
			}
			if (!output_entries_.empty() && !any_connected)
				logMessage("Outputs migration: none of the migrated outputs is connected, silent until one is connected or turned on");
		}
	}

	if (output_entries_.empty() && !settings_.containsKey("outputs"))
	{
		// First run, or nothing could be migrated: start on the Windows default device, else the preferred one.
		SoundDevice first;
		for (auto& device : active_output_devices_)
		{
			if (device.isDefaultDevice || device.isTargetedRealPlaybackDevice)
			{
				first = device;
				break;
			}
		}
		if (first.pwszID.empty())
			first = getPreferredOutput();

		if (!first.pwszID.empty())
		{
			OutputEntry entry;
			entry.id = first.pwszID.c_str();
			entry.name = first.deviceFriendlyName.c_str();
			entry.enabled = true;
			addEntry(entry);
		}

		// With no device at all, try again on the next start rather than saving an empty set (= silence).
		save = !output_entries_.empty();
	}

	for (auto* key : legacy_keys)
	{
		// While the old main output is unresolved, its three keys stay (additional_outputs is migrated already).
		if (main_pending && String(key) != "additional_outputs")
			continue;
		if (settings_.containsKey(key))
			settings_.removeValue(key);
	}

	if (save)
		saveOutputSettings();
}

void FxController::saveOutputSettings()
{
	Array<var> array;
	for (const auto& entry : output_entries_)
	{
		auto* object = new DynamicObject();
		object->setProperty("id", entry.id);
		object->setProperty("name", entry.name);
		object->setProperty("enabled", entry.enabled);
		object->setProperty("delay_ms", entry.delay_ms);
		object->setProperty("volume", entry.volume_percent);
		object->setProperty("channel_mode", static_cast<int>(entry.channel_mode));
		array.add(var(object));
	}
	settings_.setJson("outputs", var(array));
}

// Rebuilds the list of present outputs (real, active, stereo or more) in Windows device order. Returns true
// if the list changed.
bool FxController::refreshActiveOutputs(const std::vector<SoundDevice>& sound_devices)
{
	std::vector<SoundDevice> active;
	for (auto& sound_device : sound_devices)
	{
		if (sound_device.isActive && sound_device.isRealDevice && sound_device.deviceNumChannel >= 2)
			active.push_back(sound_device);
	}

	bool changed = active.size() != active_output_devices_.size();
	for (size_t i = 0; !changed && i < active.size(); i++)
	{
		changed = active[i].pwszID != active_output_devices_[i].pwszID ||
			active[i].deviceFriendlyName != active_output_devices_[i].deviceFriendlyName;
	}

	active_output_devices_ = active;

	// Keep the stored names current (shown for outputs that are not connected).
	bool renamed = false;
	for (auto& entry : output_entries_)
	{
		if (auto* device = findActiveOutput(entry.id))
		{
			String name(device->deviceFriendlyName.c_str());
			if (entry.name != name)
			{
				entry.name = name;
				renamed = true;
			}
		}
	}
	if (renamed)
		saveOutputSettings();

	return changed;
}

// "Prioritize new devices": a device never seen before is enabled when it is connected. Otherwise it
// shows up disabled.
bool FxController::enableNewOutputs(const StringArray& new_device_names)
{
	if (new_device_names.isEmpty() || !isNewOutputPrioritized())
		return false;

	bool changed = false;
	for (auto& device : active_output_devices_)
	{
		String name(device.deviceFriendlyName.c_str());
		if (!new_device_names.contains(name))
			continue;

		auto& entry = getOutputEntry(String(device.pwszID.c_str()));
		if (!entry.enabled)
		{
			entry.enabled = true;
			changed = true;
			logMessage("New output enabled: " + name);
		}
	}

	if (changed)
		saveOutputSettings();

	return changed;
}

// The carrier for the current outputs: the enabled, present output first in the device priority. With
// none, the current carrier if present (it plays silence), else the preferred device.
SoundDevice FxController::electCarrier()
{
	std::vector<SoundDevice> candidates;
	for (auto& device : active_output_devices_)
	{
		String id(device.pwszID.c_str());
		auto* entry = findOutputEntry(id);
		if (entry != nullptr && entry->enabled && !failed_carrier_ids_.contains(id))
			candidates.push_back(device);
	}

	if (!candidates.empty())
	{
		no_enabled_output_present_ = false;
		sortByDeviceConfigPriority(candidates);
		return candidates.front();
	}

	// Owner's decision: no enabled output is connected (none enabled, or the enabled ones are unplugged)
	// means silence with a notice in the UI, never a fallback to another device. The carrier stays where
	// the engine put it, so there is no new switch request and nothing can loop: it only plays silence.
	if (!no_enabled_output_present_)
	{
		no_enabled_output_present_ = true;
		logMessage("No enabled output is connected: silent until one is connected or turned on");
	}

	if (auto* current = findActiveOutput(carrier_id_))
		return *current;

	return getPreferredOutput();
}

// Elects the carrier, switches the engine to it if needed, then gives the engine the whole set of outputs.
// setAsPlaybackDevice() goes first, so the new carrier is not opened as an additional output for a moment.
void FxController::applyCarrier()
{
	if (audio_passthru_ == nullptr)
		return;

	auto carrier = electCarrier();
	if (!carrier.pwszID.empty())
		requestCarrier(carrier);

	pushOutputsToEngine();
}

void FxController::requestCarrier(const SoundDevice& device)
{
	String id(device.pwszID.c_str());
	String name(device.deviceFriendlyName.c_str());

	// While processing, the carrier is the targeted device. While off, the engine takes the Windows default
	// device when it starts.
	bool in_place = isTimerRunning() ? (id == carrier_id_) : device.isDefaultDevice;
	if (!in_place)
	{
		audio_passthru_->setAsPlaybackDevice(device);
		requested_carrier_id_ = id;
		output_changed_ = true;
		logMessage("Carrier: " + name);
	}

	// No preset change here: the carrier is internal, a re-election never replaces the user's preset.
	carrier_id_ = id;
}

String FxController::getSoleEnabledOutputId()
{
	String sole_id;
	for (auto& entry : output_entries_)
	{
		if (!entry.enabled)
			continue;
		if (sole_id.isNotEmpty())
			return {};
		sole_id = entry.id;
	}
	return sole_id;
}

// The preset chosen for a device in Output preferences is applied when the user's action (a checkbox,
// the "next output" hotkey, --output) leaves exactly that device enabled, and it was not already the only
// one. Returns the preset applied, or an empty string.
String FxController::applySoleOutputPreset(const String& previous_sole_id)
{
	auto sole_id = getSoleEnabledOutputId();
	if (sole_id.isEmpty() || sole_id == previous_sole_id)
		return {};

	String name;
	if (auto* device = findActiveOutput(sole_id))
		name = device->deviceFriendlyName.c_str();
	else if (auto* entry = findOutputEntry(sole_id))
		name = entry->name;

	auto device_config = DeviceConfig::getDeviceConfig(settings_, name);
	if (device_config.preset.isEmpty() || !setPreset(device_config.preset, false))
		return {};

	return device_config.preset;
}

void FxController::pushOutputsToEngine()
{
	if (audio_passthru_ == nullptr)
		return;

	std::vector< ::OutputConfig> outputs;
	outputs.reserve(output_entries_.size());
	for (const auto& entry : output_entries_)
	{
		::OutputConfig config;
		config.device_id = entry.id.toWideCharPointer();
		config.enabled = entry.enabled;
		config.delay_ms = entry.delay_ms;
		config.volume_percent = entry.volume_percent;
		config.channel_mode = entry.channel_mode;
		outputs.push_back(config);
	}

	audio_passthru_->setOutputs(outputs);
}

void FxController::notifyOutputsChanged()
{
	FxModel::getModel().notifyOutputListUpdated();
}

// "Next output" hotkey: enables only the present output after the first enabled one, cycling.
void FxController::enableNextOutput()
{
	String next_id;
	String next_name;
	{
		ScopedLock auto_lock(lock_);

		const auto count = active_output_devices_.size();
		if (count == 0)
			return;

		size_t current = count - 1;	// None enabled: start with the first one
		for (size_t i = 0; i < count; i++)
		{
			auto* entry = findOutputEntry(String(active_output_devices_[i].pwszID.c_str()));
			if (entry != nullptr && entry->enabled)
			{
				current = i;
				break;
			}
		}

		const auto& next = active_output_devices_[(current + 1) % count];
		next_id = next.pwszID.c_str();
		next_name = next.deviceFriendlyName.c_str();
	}

	String applied_preset;
	if (enableOnlyOutput(next_id, &applied_preset))
	{
		String message = TRANS("Output: ") + next_name;
		if (applied_preset.isNotEmpty() && FxModel::getModel().getPowerState())
			message += "\n" + TRANS("Preset: ") + applied_preset;
		FxModel::getModel().pushMessage(message);
	}
}

std::vector<FxController::OutputState> FxController::getOutputs()
{
	ScopedLock auto_lock(lock_);

	auto fill = [](OutputState& state, const OutputEntry& entry) {
		state.enabled = entry.enabled;
		state.volume_percent = entry.volume_percent;
		state.delay_ms = entry.delay_ms;
		state.channel_mode = entry.channel_mode;
	};

	std::vector<OutputState> outputs;

	// Every present output, in Windows device order...
	for (auto& device : active_output_devices_)
	{
		OutputState state;
		state.id = device.pwszID.c_str();
		state.name = device.deviceFriendlyName.c_str();
		state.present = true;
		if (auto* entry = findOutputEntry(state.id))
			fill(state, *entry);
		outputs.push_back(state);
	}

	// ...then the enabled outputs whose device is not connected.
	for (auto& entry : output_entries_)
	{
		if (!entry.enabled || findActiveOutput(entry.id) != nullptr)
			continue;

		OutputState state;
		state.id = entry.id;
		state.name = entry.name.isNotEmpty() ? entry.name : entry.id;
		state.present = false;
		fill(state, entry);
		outputs.push_back(state);
	}

	return outputs;
}

void FxController::setOutputEnabled(const String& id, bool enabled)
{
	ScopedLock auto_lock(lock_);

	// An unknown device can only be enabled while it is present.
	if (findOutputEntry(id) == nullptr && (!enabled || findActiveOutput(id) == nullptr))
		return;

	auto previous_sole_id = getSoleEnabledOutputId();

	auto& entry = getOutputEntry(id);
	if (entry.enabled == enabled)
		return;

	entry.enabled = enabled;
	saveOutputSettings();

	// Just one output left on: its device preset, if it has one.
	auto applied_preset = applySoleOutputPreset(previous_sole_id);
	if (applied_preset.isNotEmpty() && FxModel::getModel().getPowerState())
	{
		FxModel::getModel().pushMessage(TRANS("Preset: ") + applied_preset);
	}

	// The lost HDMI output was turned off: stop waiting for it.
	if (hdmi_reconnect_pending_ && !enabled && id == failover_output_id_)
	{
		clearOutputRestore();
		if (FxModel::getModel().getPowerState() && !system_sleeping_ && audio_passthru_ != nullptr)
		{
			audio_passthru_->mute(false);
		}
	}

	if (isTimerRunning() && !hdmi_reconnect_pending_ && !active_output_devices_.empty())
	{
		applyCarrier();
	}
	else
	{
		pushOutputsToEngine();
	}

	notifyOutputsChanged();
}

void FxController::setOutputVolume(const String& id, int volume_percent)
{
	ScopedLock auto_lock(lock_);

	if (findOutputEntry(id) == nullptr && findActiveOutput(id) == nullptr)
		return;

	auto& entry = getOutputEntry(id);
	volume_percent = jlimit(0, 100, volume_percent);
	if (entry.volume_percent == volume_percent)
		return;

	entry.volume_percent = volume_percent;
	saveOutputSettings();
	pushOutputsToEngine();
}

void FxController::setOutputDelay(const String& id, int delay_ms)
{
	ScopedLock auto_lock(lock_);

	if (findOutputEntry(id) == nullptr && findActiveOutput(id) == nullptr)
		return;

	auto& entry = getOutputEntry(id);
	delay_ms = jlimit(0, 500, delay_ms);
	if (entry.delay_ms == delay_ms)
		return;

	entry.delay_ms = delay_ms;
	saveOutputSettings();
	pushOutputsToEngine();
}

void FxController::setOutputChannelMode(const String& id, OutputChannelMode channel_mode)
{
	ScopedLock auto_lock(lock_);

	if (findOutputEntry(id) == nullptr && findActiveOutput(id) == nullptr)
		return;

	auto& entry = getOutputEntry(id);
	channel_mode = toOutputChannelMode(static_cast<int>(channel_mode));
	if (entry.channel_mode == channel_mode)
		return;

	entry.channel_mode = channel_mode;
	saveOutputSettings();
	pushOutputsToEngine();
}

// Enabled outputs, connected or not: the same outputs getOutputs() shows as enabled.
int FxController::getEnabledOutputCount()
{
	ScopedLock auto_lock(lock_);

	int count = 0;
	for (auto& entry : output_entries_)
	{
		if (entry.enabled)
			count++;
	}
	return count;
}

bool FxController::enableOnlyOutput(const String& id)
{
	String applied_preset;
	if (!enableOnlyOutput(id, &applied_preset))
		return false;

	if (applied_preset.isNotEmpty() && FxModel::getModel().getPowerState())
	{
		FxModel::getModel().pushMessage(TRANS("Preset: ") + applied_preset);
	}
	return true;
}

bool FxController::enableOnlyOutput(const String& id, String* applied_preset)
{
	ScopedLock auto_lock(lock_);

	if (findActiveOutput(id) == nullptr)
		return false;

	auto previous_sole_id = getSoleEnabledOutputId();

	getOutputEntry(id);
	for (auto& entry : output_entries_)
	{
		entry.enabled = (entry.id == id);
	}
	saveOutputSettings();

	auto preset = applySoleOutputPreset(previous_sole_id);
	if (applied_preset != nullptr)
		*applied_preset = preset;

	// An explicit choice ends any wait for a lost HDMI output.
	if (hdmi_reconnect_pending_)
	{
		clearOutputRestore();
		if (FxModel::getModel().getPowerState() && !system_sleeping_ && audio_passthru_ != nullptr)
		{
			audio_passthru_->mute(false);
		}
	}

	if (isTimerRunning())
	{
		applyCarrier();
	}
	else
	{
		pushOutputsToEngine();
	}

	notifyOutputsChanged();
	return true;
}
