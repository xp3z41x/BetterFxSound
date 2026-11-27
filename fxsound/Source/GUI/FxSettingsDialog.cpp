/*
FxSound
Copyright (C) 2025  FxSound LLC

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

#include <JuceHeader.h>
#include "FxSettingsDialog.h"
#include "FxMessage.h"
#include "../Utils/SysInfo/SysInfo.h"

//==============================================================================
FxSettingsDialog::FxSettingsDialog() : FxWindow("Settings"), tooltip_window_(this)
{
	setContent(&settings_content_);
	centreWithSize(getWidth(), getHeight());
	addToDesktop(0);
	toFront(true);
}

void FxSettingsDialog::closeButtonPressed()
{
	exitModalState(0);
	removeFromDesktop();
}

void FxSettingsDialog::paint(Graphics& g)
{
	FxWindow::paint(g);

	g.setColour(Colour(FXCOLOR(Outline)).withAlpha(1.0f));
	g.drawLine((float)SEPARATOR_X, (float)title_bar_.getBottom(), (float)SEPARATOR_X, (float)getLocalBounds().getBottom());
}

void FxSettingsDialog::SettingsButton::paint(Graphics& g)
{
	auto bounds = getLocalBounds();

	if (getToggleState())
	{
		g.setColour(Colour(FXCOLOR(MenuHighlightBackground)).withAlpha(1.0f));
	}
	else
	{
		g.setColour(Colour(FXCOLOR(MenuBackground)).withAlpha(1.0f));
	}
	auto rect = juce::Rectangle<float>(0, 0, (float)bounds.getHeight(), (float)bounds.getHeight());
	g.fillRoundedRectangle(rect, (float)bounds.getHeight()/4);

	image_->drawWithin(g, rect.reduced(10, 10), RectanglePlacement::centred, 1.0f);

	if (getToggleState())
	{
		g.setColour(Colour(FXCOLOR(HighlightedText)).withAlpha(1.0f));
	}
	else
	{
		g.setColour(Colour(FXCOLOR(DefaultText)).withAlpha(1.0f));
	}
	auto w = bounds.getWidth() - bounds.getHeight() + 5;

	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());
	g.setFont(theme.getNormalFont().withHeight(15.0f));
	g.drawText(TRANS(getName()), juce::Rectangle<int>(bounds.getHeight()+5, 0, w, bounds.getHeight()), Justification::centredLeft);
}

bool FxSettingsDialog::keyPressed(const KeyPress& key)
{
	if (key == KeyPress::escapeKey)
	{
		exitModalState(0);
		removeFromDesktop();
		return true;
	}

	return Component::keyPressed(key);
}

FxSettingsDialog::SettingsComponent::SettingsComponent()
{
	audio_button_ = std::make_unique<SettingsButton>("Audio");
	audio_button_->setToggleState(true, NotificationType::dontSendNotification);
	audio_button_->setImage(Drawable::createFromImageData(BinaryData::speaker_svg, BinaryData::speaker_svgSize).get());
	audio_button_->addListener(this);

	// BetterFxSound
	outputs_button_ = std::make_unique<SettingsButton>("Outputs");
	outputs_button_->setToggleState(false, NotificationType::dontSendNotification);
	outputs_button_->setImage(Drawable::createFromImageData(BinaryData::speaker_svg, BinaryData::speaker_svgSize).get());
	outputs_button_->addListener(this);

	general_button_ = std::make_unique<SettingsButton>("General");
	general_button_->setToggleState(false, NotificationType::dontSendNotification);
	general_button_->setImage(Drawable::createFromImageData(BinaryData::settings_svg, BinaryData::settings_svgSize).get());
	general_button_->addListener(this);
	
	help_button_ = std::make_unique<SettingsButton>("Help");
	help_button_->setToggleState(false, NotificationType::dontSendNotification);
	help_button_->setImage(Drawable::createFromImageData(BinaryData::question_svg, BinaryData::question_svgSize).get());
	help_button_->addListener(this);    

	addAndMakeVisible(audio_button_.get());
	addAndMakeVisible(outputs_button_.get());
	addAndMakeVisible(general_button_.get());
	addAndMakeVisible(help_button_.get());

	addAndMakeVisible(audio_settings_pane_);
	addChildComponent(outputs_settings_pane_);
	addChildComponent(general_settings_pane_);
	addChildComponent(help_settings_pane_);

    setSize(WIDTH, HEIGHT);
}

void FxSettingsDialog::SettingsComponent::resized()
{
	audio_button_->setBounds(BUTTON_X, BUTTON_Y, BUTTON_WIDTH, BUTTON_HEIGHT);
	outputs_button_->setBounds(BUTTON_X, audio_button_->getBottom() + 20, BUTTON_WIDTH, BUTTON_HEIGHT);
	general_button_->setBounds(BUTTON_X, outputs_button_->getBottom() + 20, BUTTON_WIDTH, BUTTON_HEIGHT);
	help_button_->setBounds(BUTTON_X, general_button_->getBottom() + 20, BUTTON_WIDTH, BUTTON_HEIGHT);

	juce::Rectangle<int> pane_rect(SEPARATOR_X + 1, 1, getWidth() - SEPARATOR_X + 1, getHeight() - 1);
	audio_settings_pane_.setBounds(pane_rect);
	outputs_settings_pane_.setBounds(pane_rect);
	general_settings_pane_.setBounds(pane_rect);
	help_settings_pane_.setBounds(pane_rect);
}

void  FxSettingsDialog::SettingsComponent::buttonClicked(Button* button)
{
	showPane(button);
}

void FxSettingsDialog::SettingsComponent::showPane(Button* button)
{
	struct Entry { SettingsButton* button; Component* pane; };
	Entry entries[] = {
		{ audio_button_.get(), &audio_settings_pane_ },
		{ outputs_button_.get(), &outputs_settings_pane_ },
		{ general_button_.get(), &general_settings_pane_ },
		{ help_button_.get(), &help_settings_pane_ },
	};

	bool known = false;
	for (auto& entry : entries)
	{
		if (entry.button == button)
			known = true;
	}
	if (!known)
		return;

	for (auto& entry : entries)
	{
		bool selected = (entry.button == button);
		entry.button->setToggleState(selected, NotificationType::dontSendNotification);
		entry.pane->setVisible(selected);
	}
}

FxSettingsDialog::SettingsPane::SettingsPane(String name)
{
	name_ = name;
	auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    title_.setFont(theme.getTitleFont());
	title_.setText(TRANS(name_), NotificationType::dontSendNotification);	
	title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	title_.setJustificationType(Justification::centredLeft);
	addAndMakeVisible(title_);
}

void FxSettingsDialog::SettingsPane::paint(Graphics&)
{
    auto& theme = dynamic_cast<FxTheme&>(getLookAndFeel());

    title_.setFont(theme.getTitleFont());
    title_.setText(TRANS(name_), NotificationType::dontSendNotification);    
}

FxSettingsDialog::AudioSettingsPane::AudioSettingsPane() :
	SettingsPane("Audio"),
	prioritize_new_output_toggle_(TRANS("Prioritize new output devices")),
	reset_presets_button_(TRANS("Reset presets to factory defaults"))
{
	setFocusContainerType(FocusContainerType::keyboardFocusContainer);

	output_preference_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	output_preference_title_.setJustificationType(Justification::centredLeft);

	output_preference_.setMouseCursor(MouseCursor::PointingHandCursor);
	output_preference_.setWantsKeyboardFocus(true);
	output_preference_.setEnabled(true);

	prioritize_new_output_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	prioritize_new_output_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	prioritize_new_output_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	prioritize_new_output_toggle_.setWantsKeyboardFocus(true);

	reset_presets_button_.setSize(RESET_PRESETS_BUTTON_WIDTH, BUTTON_HEIGHT);
	reset_presets_button_.setMouseCursor(MouseCursor::PointingHandCursor);

	prioritize_new_output_toggle_.setToggleState(FxController::getInstance().isNewOutputPrioritized(), NotificationType::dontSendNotification);
	prioritize_new_output_toggle_.onClick = [this]() { FxController::getInstance().setNewOutputPrioritized(prioritize_new_output_toggle_.getToggleState()); };

	auto preset_modified = false;
	auto preset_count = FxModel::getModel().getPresetCount();
	for (auto i = 0; i < preset_count; i++)
	{
		if (FxModel::getModel().isPresetModified(i))
		{
			preset_modified = true;
			break;
		}
	}
	reset_presets_button_.setEnabled(FxModel::getModel().getUserPresetCount() > 0 || preset_modified);

	reset_presets_button_.onClick = [this]() {
		auto& controller = FxController::getInstance();

		controller.resetPresets();
		reset_presets_button_.setEnabled(false);
		};

	setText();

	addAndMakeVisible(&output_preference_title_);
	addAndMakeVisible(&output_preference_);
	addAndMakeVisible(&prioritize_new_output_toggle_);
	addAndMakeVisible(&reset_presets_button_);
}

FxSettingsDialog::AudioSettingsPane::~AudioSettingsPane()
{
}

void FxSettingsDialog::AudioSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	output_preference_title_.setBounds(X_MARGIN, ENDPOINT_Y, LABEL_WIDTH, LABEL_HEIGHT);
	int y = output_preference_title_.getBottom() + 10;
	auto width = getWidth() - ((X_MARGIN + 5) * 2);
	output_preference_.setBounds(X_MARGIN, y, width, OUTPUT_PREFERENCE_HEIGHT);

    y = output_preference_.getBottom() + 10;
    prioritize_new_output_toggle_.setBounds(X_MARGIN, y, width, TOGGLE_BUTTON_HEIGHT);

	auto group_x = static_cast<float>(output_preference_title_.getX() - GROUP_MARGIN);
	auto group_y = static_cast<float>(output_preference_title_.getY() - GROUP_MARGIN);
	auto group_width = static_cast<float>(output_preference_.getRight() - group_x + GROUP_MARGIN);
	auto group_height = static_cast<float>(prioritize_new_output_toggle_.getBottom() - group_y + GROUP_MARGIN);
	output_preference_bounds_ = juce::Rectangle<float>(group_x, group_y, group_width, group_height);

	y = prioritize_new_output_toggle_.getBottom() + 30;
	resizeResetButton(X_MARGIN, y);
}

void FxSettingsDialog::AudioSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

	g.setFillType(FillType(Colour(FXCOLOR(DefaultFill)).withAlpha(0.2f)));
	g.fillRoundedRectangle(output_preference_bounds_, 8);

	setText();

	SettingsPane::paint(g);
}

void FxSettingsDialog::AudioSettingsPane::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

	output_preference_title_.setFont(theme.getNormalFont().withHeight(15.0f));
	output_preference_title_.setText(TRANS("Output Device Preference"), NotificationType::dontSendNotification);

	prioritize_new_output_toggle_.setButtonText(TRANS("Prioritize new output devices"));

	reset_presets_button_.setButtonText(TRANS("Reset presets to factory defaults"));
	resizeResetButton(reset_presets_button_.getX(), reset_presets_button_.getY());
}

void FxSettingsDialog::AudioSettingsPane::resizeResetButton(int x, int y)
{
	String buttonText = reset_presets_button_.getButtonText();

	int index = 0;
	int lineCount = 1;
	do {
		index = buttonText.indexOfChar(index, L'\n');
		if (index >= 0)
		{
			index++;
			lineCount++;
		}
		else
		{
			break;
		}
	} while (lineCount <= 3); // Resize the button height for upto 3 lines of text

	int buttonWidth = min(reset_presets_button_.getBestWidthForHeight(BUTTON_HEIGHT * lineCount), MAX_BUTTON_WIDTH);
	if (buttonWidth < RESET_PRESETS_BUTTON_WIDTH)
	{
		buttonWidth = RESET_PRESETS_BUTTON_WIDTH;
	}

	reset_presets_button_.setBounds(x, y, buttonWidth, BUTTON_HEIGHT * lineCount);
}

void FxSettingsDialog::AudioSettingsPane::visibilityChanged()
{
	if (isVisible())
	{
		output_preference_.update();
    }
}

void FxSettingsDialog::AudioSettingsPane::mouseEnter(const MouseEvent& mouse_event)
{
	Component::mouseEnter(mouse_event);
}

void FxSettingsDialog::AudioSettingsPane::mouseExit(const MouseEvent& mouse_event)
{
	Component::mouseEnter(mouse_event);
}

// BetterFxSound: additional outputs pane.
FxSettingsDialog::OutputsSettingsPane::OutputsSettingsPane() :
	SettingsPane("Additional Outputs")
{
	setFocusContainerType(FocusContainerType::keyboardFocusContainer);

	description_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	description_.setJustificationType(Justification::topLeft);
	description_.setMinimumHorizontalScale(1.0f);

	empty_label_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId).withAlpha(0.7f));
	empty_label_.setJustificationType(Justification::centred);

	viewport_.setViewedComponent(&rows_container_, false);
	viewport_.setScrollBarsShown(true, false);

	setText();

	addAndMakeVisible(description_);
	addChildComponent(empty_label_);
	addAndMakeVisible(viewport_);
}

void FxSettingsDialog::OutputsSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	auto width = getWidth() - ((X_MARGIN + 5) * 2);
	description_.setBounds(X_MARGIN, DESCRIPTION_Y, width, DESCRIPTION_HEIGHT);

	auto list_y = description_.getBottom() + 10;
	auto list_height = jmax(60, getHeight() - list_y - 20);
	viewport_.setBounds(X_MARGIN, list_y, width, list_height);
	empty_label_.setBounds(viewport_.getBounds());

	int total_height = 0;
	for (auto& row : rows_)
		total_height += row->getPreferredHeight() + ROW_SPACING;

	auto row_width = width;
	if (total_height > list_height)
		row_width = width - viewport_.getScrollBarThickness();

	rows_container_.setSize(row_width, total_height);
	int y = 0;
	for (auto& row : rows_)
	{
		auto height = row->getPreferredHeight();
		row->setBounds(0, y, row_width, height);
		y += height + ROW_SPACING;
	}
}

void FxSettingsDialog::OutputsSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

	setText();

	SettingsPane::paint(g);
}

void FxSettingsDialog::OutputsSettingsPane::setText()
{
	auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

	description_.setFont(theme.getNormalFont().withHeight(14.0f));
	description_.setText(TRANS("Play the enhanced sound on these devices at the same time as the main output. "
		"Each device can have its own volume and play both channels, only the left or right one, or mono. "
		"Use the delay to line the devices up: if one device sounds late (for example Bluetooth headphones), "
		"make that one the main output and add delay to the others."), NotificationType::dontSendNotification);

	empty_label_.setFont(theme.getNormalFont().withHeight(14.0f));
	empty_label_.setText(TRANS("No other output devices were found."), NotificationType::dontSendNotification);
}

void FxSettingsDialog::OutputsSettingsPane::visibilityChanged()
{
	if (isVisible())
	{
		rebuild();
	}
}

void FxSettingsDialog::OutputsSettingsPane::rebuild()
{
	auto& controller = FxController::getInstance();
	auto additional_outputs = controller.getAdditionalOutputs();
	auto main_channel_mode = controller.getMainOutputChannelMode();
	auto main_output = FxModel::getModel().getSelectedOutput();

	rows_.clear();

	for (auto& device : FxModel::getModel().getOutputDevices())
	{
		if (!device.isRealDevice || device.isDFXDevice || device.pwszID.empty())
			continue;

		bool is_main = (device.pwszID == main_output.pwszID);
		const AdditionalOutputConfig* config = nullptr;
		for (auto& output : additional_outputs)
		{
			if (output.device_id == device.pwszID)
				config = &output;
		}

		auto row = std::make_unique<OutputRow>(device, is_main, config, main_channel_mode);
		row->onLayoutChanged = [this]() { resized(); };
		rows_container_.addAndMakeVisible(row.get());
		rows_.push_back(std::move(row));
	}

	empty_label_.setVisible(rows_.size() <= 1);
	viewport_.setVisible(rows_.size() > 1);

	resized();
}

FxSettingsDialog::OutputsSettingsPane::OutputRow::OutputRow(const SoundDevice& device, bool is_main_output, const AdditionalOutputConfig* config, OutputChannelMode main_channel_mode) :
	device_id_(device.pwszID.c_str()),
	is_main_output_(is_main_output)
{
	auto text_colour = getLookAndFeel().findColour(TextButton::textColourOnId);
	auto name = String(device.deviceFriendlyName.c_str());
	if (is_main_output_)
		name += " " + TRANS("(main output)");

	bool enabled = (config != nullptr);
	int delay_ms = enabled ? config->delay_ms : 0;
	int volume = enabled ? config->volume_percent : 100;
	auto channel_mode = is_main_output_ ? main_channel_mode : (enabled ? config->channel_mode : OutputChannelMode::Both);

	toggle_.setButtonText(name);
	toggle_.setColour(ToggleButton::ColourIds::tickColourId, text_colour);
	toggle_.setColour(ToggleButton::ColourIds::textColourId, text_colour);
	toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	toggle_.setWantsKeyboardFocus(true);
	toggle_.setToggleState(is_main_output_ || enabled, NotificationType::dontSendNotification);
	toggle_.setEnabled(!is_main_output_);
	toggle_.onClick = [this]() {
		updateControlsState();
		save();
		if (onLayoutChanged)
			onLayoutChanged();
	};

	// Item ids are OutputChannelMode + 1 (ComboBox ids must not be 0).
	channel_list_.addItem(TRANS("Both channels"), static_cast<int>(OutputChannelMode::Both) + 1);
	channel_list_.addItem(TRANS("Left only"), static_cast<int>(OutputChannelMode::Left) + 1);
	channel_list_.addItem(TRANS("Right only"), static_cast<int>(OutputChannelMode::Right) + 1);
	channel_list_.addItem(TRANS("Mono"), static_cast<int>(OutputChannelMode::Mono) + 1);
	channel_list_.setSelectedId(static_cast<int>(channel_mode) + 1, NotificationType::dontSendNotification);
	channel_list_.setMouseCursor(MouseCursor::PointingHandCursor);
	channel_list_.setTooltip(TRANS("Which channels this device plays"));
	channel_list_.onChange = [this]() { save(); };

	delay_label_.setText(TRANS("Delay"), NotificationType::dontSendNotification);
	delay_label_.setColour(Label::ColourIds::textColourId, text_colour);
	delay_label_.setJustificationType(Justification::centredLeft);
	setupSlider(delay_slider_, 500.0, " ms", text_colour);
	delay_slider_.setValue(delay_ms, NotificationType::dontSendNotification);
	delay_slider_.onDragEnd = [this]() { save(); };
	delay_slider_.onValueChange = [this]() {
		// Typed values and arrow keys do not produce a drag end, save those right away.
		if (!delay_slider_.isMouseButtonDown())
			save();
	};

	volume_label_.setText(TRANS("Volume"), NotificationType::dontSendNotification);
	volume_label_.setColour(Label::ColourIds::textColourId, text_colour);
	volume_label_.setJustificationType(Justification::centredLeft);
	setupSlider(volume_slider_, 100.0, " %", text_colour);
	volume_slider_.setValue(volume, NotificationType::dontSendNotification);
	// Volume is applied live while dragging so it can be set by ear.
	volume_slider_.onValueChange = [this]() { save(); };

	addAndMakeVisible(toggle_);
	addAndMakeVisible(channel_list_);
	addAndMakeVisible(delay_label_);
	addAndMakeVisible(delay_slider_);
	addAndMakeVisible(volume_label_);
	addAndMakeVisible(volume_slider_);

	updateControlsState();
}

void FxSettingsDialog::OutputsSettingsPane::OutputRow::setupSlider(Slider& slider, double max_value, const String& suffix, Colour text_colour)
{
	slider.setSliderStyle(Slider::SliderStyle::LinearHorizontal);
	slider.setTextBoxStyle(Slider::TextBoxRight, false, 70, 20);
	slider.setRange(0.0, max_value, max_value > 100.0 ? 5.0 : 1.0);
	slider.setTextValueSuffix(suffix);
	slider.setColour(Slider::ColourIds::textBoxTextColourId, text_colour);
	slider.setMouseCursor(MouseCursor::PointingHandCursor);
}

int FxSettingsDialog::OutputsSettingsPane::OutputRow::getPreferredHeight() const
{
	// One line for the name, plus delay and volume when this additional output is on.
	bool show_sliders = !is_main_output_ && toggle_.getToggleState();
	return 8 + LINE_HEIGHT * (show_sliders ? 3 : 1);
}

void FxSettingsDialog::OutputsSettingsPane::OutputRow::resized()
{
	auto bounds = getLocalBounds().reduced(8, 4);

	auto top = bounds.removeFromTop(LINE_HEIGHT);
	channel_list_.setBounds(top.removeFromRight(120).reduced(0, 2));
	toggle_.setBounds(top.withTrimmedRight(6));

	auto delay_bounds = bounds.removeFromTop(LINE_HEIGHT).withTrimmedLeft(28);
	delay_label_.setBounds(delay_bounds.removeFromLeft(60));
	delay_slider_.setBounds(delay_bounds);

	auto volume_bounds = bounds.removeFromTop(LINE_HEIGHT).withTrimmedLeft(28);
	volume_label_.setBounds(volume_bounds.removeFromLeft(60));
	volume_slider_.setBounds(volume_bounds);
}

void FxSettingsDialog::OutputsSettingsPane::OutputRow::paint(Graphics& g)
{
	g.setColour(Colour(FXCOLOR(DefaultFill)).withAlpha(0.2f));
	g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
}

void FxSettingsDialog::OutputsSettingsPane::OutputRow::updateControlsState()
{
	bool active = is_main_output_ || toggle_.getToggleState();
	bool show_sliders = !is_main_output_ && toggle_.getToggleState();

	channel_list_.setVisible(active);
	delay_label_.setVisible(show_sliders);
	delay_slider_.setVisible(show_sliders);
	volume_label_.setVisible(show_sliders);
	volume_slider_.setVisible(show_sliders);
}

void FxSettingsDialog::OutputsSettingsPane::OutputRow::save()
{
	auto channel_mode = static_cast<OutputChannelMode>(jlimit(0, 3, channel_list_.getSelectedId() - 1));
	auto& controller = FxController::getInstance();

	if (is_main_output_)
	{
		controller.setMainOutputChannelMode(channel_mode);
		return;
	}

	controller.setAdditionalOutput(device_id_, toggle_.getToggleState(),
		static_cast<int>(delay_slider_.getValue()), static_cast<int>(volume_slider_.getValue()), channel_mode);
}

FxSettingsDialog::GeneralSettingsPane::GeneralSettingsPane() :
	SettingsPane("General Preferences"),
	launch_toggle_(TRANS("Launch on system startup")),
	hide_help_tips_toggle_(TRANS("Hide help tips for audio controls")),
	hide_notifications_toggle_(TRANS("Hide notifications")),
	gpu_render_toggle_(TRANS("Use hardware-accelerated graphics")),
	hotkeys_toggle_(TRANS("Disable keyboard shortcuts"))
{
	StringArray hotKeySettingsKeys = { FxController::HK_CMD_ON_OFF, FxController::HK_CMD_OPEN_CLOSE, FxController::HK_CMD_NEXT_PRESET, FxController::HK_CMD_PREVIOUS_PRESET, FxController::HK_CMD_NEXT_OUTPUT };
	StringArray hotkey_names = { "Turn FxSound On/Off", "Open/Close FxSound",
								   "Use Next Preset", "Use Previous Preset", "Change Playback Device"};

	setFocusContainerType(FocusContainerType::keyboardFocusContainer);

	launch_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	launch_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	launch_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	launch_toggle_.setWantsKeyboardFocus(true);
	
	hide_help_tips_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
    hide_help_tips_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
    hide_help_tips_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_help_tips_toggle_.setWantsKeyboardFocus(true);
	hide_notifications_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	hide_notifications_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_notifications_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hide_notifications_toggle_.setWantsKeyboardFocus(true);
	gpu_render_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	gpu_render_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	gpu_render_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	gpu_render_toggle_.setWantsKeyboardFocus(true);
	hotkeys_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	hotkeys_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hotkeys_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	hotkeys_toggle_.setWantsKeyboardFocus(true);

	bool hotkey_enabled = FxModel::getModel().getHotkeySupport();
	for (int i=0; i<hotkey_names.size(); i++)
	{
		auto label = new FxHotkeyLabel(hotkey_names[i], hotKeySettingsKeys[i]);
		label->setEnabled(hotkey_enabled);
		hotkey_labels_.add(label);
		addAndMakeVisible(label);
	}

    if (SysInfo::canSupportHotkeys())
    {
        hotkeys_toggle_.setToggleState(!FxModel::getModel().getHotkeySupport(), NotificationType::dontSendNotification);
    }
    else
    {
        hotkeys_toggle_.setToggleState(true, NotificationType::dontSendNotification);
        hotkeys_toggle_.setEnabled(false);
    }
	hotkeys_toggle_.onClick = [this]()  { 
											FxController::getInstance().enableHotkeys(!hotkeys_toggle_.getToggleState());
											bool enabled = FxModel::getModel().getHotkeySupport();
											for (auto& hotkey_label : hotkey_labels_)
											{
												hotkey_label->setEnabled(enabled);
											}
										};

	launch_toggle_.setToggleState(FxController::getInstance().isLaunchOnStartup(), NotificationType::dontSendNotification);
	launch_toggle_.onClick = [this]() { FxController::getInstance().setLaunchOnStartup(launch_toggle_.getToggleState()); };

    hide_help_tips_toggle_.setToggleState(FxController::getInstance().isHelpTooltipsHidden(), NotificationType::dontSendNotification);
    hide_help_tips_toggle_.onClick = [this]() { FxController::getInstance().setHelpTooltipsHidden(hide_help_tips_toggle_.getToggleState()); };
	
	hide_notifications_toggle_.setToggleState(FxController::getInstance().isNotificationsHidden(), NotificationType::dontSendNotification);
	hide_notifications_toggle_.onClick = [this]() { FxController::getInstance().setNotificationsHidden(hide_notifications_toggle_.getToggleState()); };

	gpu_render_toggle_.setToggleState(FxController::getInstance().isGpuRenderingEnabled(), NotificationType::dontSendNotification);
	gpu_render_toggle_.onClick = [this]() { onGpuRenderToggled(); };

	language_switch_.setSelectedLanguage(FxController::getInstance().getLanguage());
	language_switch_.onLanguageChanged = [](String language_code) { FxController::getInstance().setLanguage(language_code); };

	auto os = SystemStats::getOperatingSystemType();
	if (os == SystemStats::OperatingSystemType::Windows7)
	{
		addAndMakeVisible(&launch_toggle_);
	}
	
	addAndMakeVisible(&hide_help_tips_toggle_);
	addAndMakeVisible(&hide_notifications_toggle_);
	if (FxController::canSupportGpuRendering())
	{
		addAndMakeVisible(&gpu_render_toggle_);
	}
	addAndMakeVisible(&hotkeys_toggle_);
	addAndMakeVisible(&language_switch_);

	setText();
}

FxSettingsDialog::GeneralSettingsPane::~GeneralSettingsPane()
{
}

void FxSettingsDialog::GeneralSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

    language_switch_.setBounds(X_MARGIN, LANGUAGE_SWITCH_Y, FxLanguage::WIDTH, FxLanguage::HEIGHT);

    int y = language_switch_.getBottom() + 20;
	if (launch_toggle_.isVisible())
	{
		launch_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);
		y = launch_toggle_.getBottom() + 20;
	}

    hide_help_tips_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);

	y = hide_help_tips_toggle_.getBottom() + 10;
	hide_notifications_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);

    y = hide_notifications_toggle_.getBottom() + 10;
	if (gpu_render_toggle_.isVisible())
	{
		gpu_render_toggle_.setBounds(X_MARGIN, y, getWidth() - X_MARGIN, TOGGLE_BUTTON_HEIGHT);
		y = gpu_render_toggle_.getBottom() + 10;
	}

	hotkeys_toggle_.setBounds(X_MARGIN, y, getWidth()-X_MARGIN, TOGGLE_BUTTON_HEIGHT);

	y = hotkeys_toggle_.getBottom() + 5;
	for (auto hotkey_label : hotkey_labels_)
	{
		hotkey_label->setBounds(HOTKEY_LABEL_X, y, getWidth()-HOTKEY_LABEL_X, HOTKEY_LABEL_HEIGHT);
		y += HOTKEY_LABEL_HEIGHT + 10;
	}
}

void FxSettingsDialog::GeneralSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

    setText();

	SettingsPane::paint(g);    
}

void FxSettingsDialog::GeneralSettingsPane::setText()
{
    launch_toggle_.setButtonText(TRANS("Launch on system startup"));
    hide_help_tips_toggle_.setButtonText(TRANS("Hide help tips for audio controls"));
	hide_notifications_toggle_.setButtonText(TRANS("Hide notifications"));
	gpu_render_toggle_.setButtonText(TRANS("Use hardware-accelerated graphics"));

    hotkeys_toggle_.setButtonText(TRANS("Disable keyboard shortcuts"));
}

void FxSettingsDialog::GeneralSettingsPane::onGpuRenderToggled()
{
	auto& controller = FxController::getInstance();
	bool enable = gpu_render_toggle_.getToggleState();

	controller.setGpuRenderingEnabled(enable);

	// Switching back to the engine already running needs no restart.
	if (enable == controller.isGpuRenderingActive())
	{
		return;
	}

	FxConfirmationMessage::showMessage(TRANS("FxSound needs to restart to apply this change. Restart now?"), FxConfirmationMessage::Style::YesNo,
		[](bool restart)
		{
            if (restart)
            {
                FxController::getInstance().restartApplication();
            }
		},
		TRANS("Restart now"), TRANS("Later"));
}

FxSettingsDialog::HelpSettingsPane::HelpSettingsPane() : SettingsPane("Help"), auto_updates_toggle_(TRANS("Automatic updates"))
{	
	version_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	version_title_.setJustificationType(Justification::centredLeft);
	version_text_.setJustificationType(Justification::centredLeft);	
	support_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	support_title_.setJustificationType(Justification::centredLeft);	
	maintenance_title_.setColour(Label::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	maintenance_title_.setJustificationType(Justification::centredLeft);

	changelog_link_.setURL(URL(L"https://www.fxsound.com/changelog"));
	changelog_link_.setJustificationType(Justification::topLeft);
	quicktour_link_.setJustificationType(Justification::topLeft);
	submitlogs_link_.setJustificationType(Justification::topLeft);
	helpcenter_link_.setURL(URL(L"https://www.fxsound.com/learning-center"));
	helpcenter_link_.setJustificationType(Justification::topLeft);
    feedback_link_.setURL(URL("https://james722808.typeform.com/to/QfEP5QrP"));
	feedback_link_.setJustificationType(Justification::topLeft);

	auto_updates_toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	auto_updates_toggle_.setToggleState(FxController::getInstance().getAutoUpdates(), NotificationType::dontSendNotification);
	auto_updates_toggle_.setColour(ToggleButton::ColourIds::tickColourId, getLookAndFeel().findColour(TextButton::textColourOnId));
	auto_updates_toggle_.setColour(ToggleButton::ColourIds::textColourId, getLookAndFeel().findColour(TextButton::textColourOnId));

	auto_updates_toggle_.onClick = [this]() {
		FxController::getInstance().setAutoUpdates(auto_updates_toggle_.getToggleState());
	};

    setText();

	addAndMakeVisible(version_title_);
	addAndMakeVisible(version_text_);
	addAndMakeVisible(support_title_);
	addAndMakeVisible(maintenance_title_);
	addAndMakeVisible(changelog_link_);
	addChildComponent(quicktour_link_);
	addChildComponent(submitlogs_link_);
	addAndMakeVisible(helpcenter_link_);
	addAndMakeVisible(auto_updates_toggle_);
}

void FxSettingsDialog::HelpSettingsPane::resized()
{
	auto bounds = getLocalBounds().withLeft(X_MARGIN).withTop(Y_MARGIN).withHeight(TITLE_HEIGHT);
	title_.setBounds(bounds);

	version_title_.setBounds(X_MARGIN, TEXT_Y, getWidth()-X_MARGIN, TITLE_HEIGHT);
	version_text_.setBounds(X_MARGIN, version_title_.getBottom()+10, getWidth()-X_MARGIN, TEXT_HEIGHT);
	changelog_link_.setBounds(X_MARGIN+5, version_text_.getBottom()+10, getWidth()-X_MARGIN, HYPERLINK_HEIGHT);
	support_title_.setBounds(X_MARGIN, changelog_link_.getBottom()+20, getWidth()-X_MARGIN, TITLE_HEIGHT);
	helpcenter_link_.setBounds(X_MARGIN+5, support_title_.getBottom()+10, getWidth()-X_MARGIN, HYPERLINK_HEIGHT);
	maintenance_title_.setBounds(X_MARGIN, helpcenter_link_.getBottom()+20, getWidth()-X_MARGIN, TITLE_HEIGHT);
	auto_updates_toggle_.setBounds(X_MARGIN + 5, maintenance_title_.getBottom() + 10, BUTTON_WIDTH, TOGGLE_BUTTON_HEIGHT);
}

void FxSettingsDialog::HelpSettingsPane::paint(Graphics& g)
{
	g.fillAll(getLookAndFeel().findColour(ResizableWindow::backgroundColourId));

    setText();

	SettingsPane::paint(g);
}

void FxSettingsDialog::HelpSettingsPane::setText()
{
    auto& theme = dynamic_cast<FxTheme&>(LookAndFeel::getDefaultLookAndFeel());

    version_title_.setText(TRANS("Version"), NotificationType::dontSendNotification);
    version_title_.setFont(theme.getNormalFont());
    
    version_text_.setText(L"v" + JUCEApplication::getInstance()->getApplicationVersion(), NotificationType::dontSendNotification);
    version_text_.setFont(theme.getSmallFont());
    
    support_title_.setText(TRANS("Support"), NotificationType::dontSendNotification);
    support_title_.setFont(theme.getNormalFont());
    
    maintenance_title_.setText(TRANS("Maintenance"), NotificationType::dontSendNotification);
    maintenance_title_.setFont(theme.getNormalFont());

    changelog_link_.setButtonText(TRANS("Changelog"));    
    quicktour_link_.setButtonText(TRANS("Quick tour"));    
    submitlogs_link_.setButtonText(TRANS("Submit debug logs"));    
    helpcenter_link_.setButtonText(TRANS("Help center"));        
    feedback_link_.setButtonText(TRANS("Feedback"));
	auto_updates_toggle_.setButtonText(TRANS("Automatic updates"));;
}
