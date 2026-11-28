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

#include "FxOutputsGrid.h"
#include "FxTheme.h"

namespace
{
	// Middle dot separator used in the tile summary line.
	String getSeparator()
	{
		return String(CharPointer_UTF8("  \xc2\xb7  "));
	}

	Colour getThemeColour(FxColor color, float alpha = 1.0f)
	{
		return Colour(FxTheme::getColor(color)).withAlpha(alpha);
	}
}

//==============================================================================
// FxOutputSettingsPanel
//==============================================================================
FxOutputSettingsPanel::FxOutputSettingsPanel(const FxController::OutputState& output) :
	output_(output), tooltip_window_(this)
{
	title_.setText(output_.name, NotificationType::dontSendNotification);
	title_.setJustificationType(Justification::centredLeft);
	title_.setMinimumHorizontalScale(0.7f);
	title_.setTooltip(output_.name);

	volume_label_.setText(TRANS("Volume"), NotificationType::dontSendNotification);
	setupSlider(volume_slider_, 100.0, 1.0, " %");
	volume_slider_.setValue(output_.volume_percent, NotificationType::dontSendNotification);
	volume_slider_.setDoubleClickReturnValue(true, 100.0);
	volume_slider_.setTooltip(TRANS("Volume of this output, on top of the Windows volume"));
	// Volume is applied live while dragging so it can be set by ear.
	volume_slider_.onValueChange = [this]() {
		output_.volume_percent = static_cast<int>(volume_slider_.getValue());
		FxController::getInstance().setOutputVolume(output_.id, output_.volume_percent);
		notifyChanged();
	};

	delay_label_.setText(TRANS("Delay"), NotificationType::dontSendNotification);
	// 1 ms steps: delays come from the microphone sync to the millisecond (12, 69, 185...). A
	// coarser step would show a rounded value and, as the panel saves the shown value when it
	// closes, overwrite the measured delay just by opening and closing the panel.
	setupSlider(delay_slider_, 500.0, 1.0, " ms");
	delay_slider_.setValue(output_.delay_ms, NotificationType::dontSendNotification);
	delay_slider_.setDoubleClickReturnValue(true, 0.0);
	delay_slider_.setTooltip(TRANS("Delays this output to line it up with the others"));
	delay_slider_.onDragEnd = [this]() { saveDelay(); };
	delay_slider_.onValueChange = [this]() {
		// Typed values and arrow keys do not produce a drag end, save those right away.
		if (!delay_slider_.isMouseButtonDown())
			saveDelay();
	};

	channel_label_.setText(TRANS("Channels"), NotificationType::dontSendNotification);
	// Item ids are OutputChannelMode + 1 (ComboBox ids must not be 0).
	for (auto mode : { OutputChannelMode::Both, OutputChannelMode::Left, OutputChannelMode::Right, OutputChannelMode::Mono })
	{
		channel_list_.addItem(FxOutputsGrid::getChannelModeText(mode), static_cast<int>(mode) + 1);
	}
	channel_list_.setSelectedId(static_cast<int>(output_.channel_mode) + 1, NotificationType::dontSendNotification);
	channel_list_.setMouseCursor(MouseCursor::PointingHandCursor);
	channel_list_.setTooltip(TRANS("Which channels this device plays"));
	channel_list_.onChange = [this]() {
		output_.channel_mode = static_cast<OutputChannelMode>(jlimit(0, 3, channel_list_.getSelectedId() - 1));
		FxController::getInstance().setOutputChannelMode(output_.id, output_.channel_mode);
		notifyChanged();
	};

	for (auto* label : { &volume_label_, &delay_label_, &channel_label_ })
	{
		label->setJustificationType(Justification::centredLeft);
		addAndMakeVisible(label);
	}
	addAndMakeVisible(title_);
	addAndMakeVisible(volume_slider_);
	addAndMakeVisible(delay_slider_);
	addAndMakeVisible(channel_list_);

	applyTheme();
	setSize(WIDTH, HEIGHT);

	FxModel::getModel().addListener(this);
}

FxOutputSettingsPanel::~FxOutputSettingsPanel()
{
	FxModel::getModel().removeListener(this);

	// The callout can be closed in the middle of an edit, keep the last delay shown.
	if (static_cast<int>(delay_slider_.getValue()) != output_.delay_ms)
		saveDelay();
}

void FxOutputSettingsPanel::show(const FxController::OutputState& output, Component& target,
								 std::function<void(const FxController::OutputState&)> on_changed)
{
	auto panel = std::make_unique<FxOutputSettingsPanel>(output);
	panel->onChanged = std::move(on_changed);
	CallOutBox::launchAsynchronously(std::move(panel), target.getScreenBounds(), nullptr);
}

void FxOutputSettingsPanel::modelChanged(FxModel::Event model_event)
{
	if (model_event != FxModel::Event::OutputListUpdated)
		return;

	for (auto& output : FxController::getInstance().getOutputs())
	{
		if (output.id != output_.id)
			continue;

		// Leave a slider alone while the user drags it, the drag sets the value anyway.
		if (!volume_slider_.isMouseButtonDown())
		{
			output_.volume_percent = output.volume_percent;
			volume_slider_.setValue(output.volume_percent, NotificationType::dontSendNotification);
		}
		if (!delay_slider_.isMouseButtonDown())
		{
			output_.delay_ms = output.delay_ms;
			delay_slider_.setValue(output.delay_ms, NotificationType::dontSendNotification);
		}
		output_.channel_mode = output.channel_mode;
		channel_list_.setSelectedId(static_cast<int>(output.channel_mode) + 1, NotificationType::dontSendNotification);
		output_.enabled = output.enabled;
		output_.present = output.present;
		output_.failed = output.failed;
		return;
	}

	// The output is not listed anymore (a disabled device was unplugged): close the panel.
	// dismiss() is asynchronous, so the panel is not deleted while the model notifies.
	if (auto* callout = findParentComponentOfClass<CallOutBox>())
		callout->dismiss();
}

void FxOutputSettingsPanel::setupSlider(Slider& slider, double max_value, double interval, const String& suffix)
{
	slider.setSliderStyle(Slider::SliderStyle::LinearHorizontal);
	slider.setTextBoxStyle(Slider::TextBoxRight, false, 64, 20);
	slider.setRange(0.0, max_value, interval);
	slider.setTextValueSuffix(suffix);
	slider.setMouseCursor(MouseCursor::PointingHandCursor);
	slider.setWantsKeyboardFocus(true);
}

void FxOutputSettingsPanel::saveDelay()
{
	auto delay_ms = static_cast<int>(delay_slider_.getValue());
	if (delay_ms == output_.delay_ms)
		return;

	output_.delay_ms = delay_ms;
	FxController::getInstance().setOutputDelay(output_.id, delay_ms);
	notifyChanged();
}

void FxOutputSettingsPanel::notifyChanged()
{
	if (onChanged)
		onChanged(output_);
}

void FxOutputSettingsPanel::resized()
{
	auto bounds = getLocalBounds().reduced(MARGIN);

	title_.setBounds(bounds.removeFromTop(22));
	bounds.removeFromTop(4);

	auto row = bounds.removeFromTop(ROW_HEIGHT);
	volume_label_.setBounds(row.removeFromLeft(LABEL_WIDTH));
	volume_slider_.setBounds(row);

	row = bounds.removeFromTop(ROW_HEIGHT);
	delay_label_.setBounds(row.removeFromLeft(LABEL_WIDTH));
	delay_slider_.setBounds(row);

	row = bounds.removeFromTop(ROW_HEIGHT);
	channel_label_.setBounds(row.removeFromLeft(LABEL_WIDTH));
	channel_list_.setBounds(row.removeFromLeft(150).reduced(0, 3));
}

void FxOutputSettingsPanel::paint(Graphics&)
{
	// The CallOutBox draws the background.
}

void FxOutputSettingsPanel::lookAndFeelChanged()
{
	applyTheme();
}

void FxOutputSettingsPanel::applyTheme()
{
	auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel());
	auto text_colour = getThemeColour(FxColor::DefaultText);

	title_.setColour(Label::ColourIds::textColourId, getThemeColour(FxColor::HighlightedText));
	for (auto* label : { &volume_label_, &delay_label_, &channel_label_ })
	{
		label->setColour(Label::ColourIds::textColourId, text_colour);
		if (theme != nullptr)
			label->setFont(theme->getSmallFont().withHeight(13.0f));
	}
	if (theme != nullptr)
		title_.setFont(theme->getNormalFont());

	for (auto* slider : { &volume_slider_, &delay_slider_ })
	{
		slider->setColour(Slider::ColourIds::textBoxTextColourId, text_colour);
		slider->setColour(Slider::ColourIds::textBoxOutlineColourId, Colours::transparentBlack);
		slider->setColour(Slider::ColourIds::textBoxBackgroundColourId, Colours::transparentBlack);
	}
}

//==============================================================================
// FxOutputsGrid::Tile
//==============================================================================
FxOutputsGrid::Tile::Tile() : gear_button_("outputSettingsButton", DrawableButton::ButtonStyle::ImageFitted)
{
	toggle_.setMouseCursor(MouseCursor::PointingHandCursor);
	toggle_.setWantsKeyboardFocus(true);
	toggle_.onClick = [this]() {
		output_.enabled = toggle_.getToggleState();
		repaint();
		if (onEnabledChanged)
			onEnabledChanged(output_);
	};
	toggle_.addMouseListener(this, false);

	gear_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	gear_button_.setWantsKeyboardFocus(true);
	gear_button_.setTooltip(TRANS("Volume, delay and channels of this output"));
	gear_button_.setTitle(TRANS("Output settings"));
	gear_button_.onClick = [this]() {
		if (onGearClicked)
			onGearClicked(*this);
	};
	gear_button_.addMouseListener(this, false);

	addAndMakeVisible(toggle_);
	addAndMakeVisible(gear_button_);

	applyTheme();
}

void FxOutputsGrid::Tile::setOutput(const FxController::OutputState& output)
{
	output_ = output;

	toggle_.setToggleState(output_.enabled, NotificationType::dontSendNotification);
	toggle_.setTitle(output_.name);

	String tooltip = output_.name;
	if (!output_.present)
		tooltip += "\n" + TRANS("Disconnected, this output does not play");
	else if (output_.failed)
		tooltip += "\n" + TRANS("FxSound could not open this device, this output does not play. Another app may be using it exclusively. FxSound keeps trying.");
	// BetterFxSound: an enabled output's device is held at 100% and the Windows volume is applied by FxSound to the
	// sound it plays. An app set to play straight to this device (its own device setting, or the communications
	// default when that is not FxSound) bypasses both, so say so where the output is chosen.
	else if (output_.enabled)
		tooltip += "\n" + TRANS("Apps set to play straight to this device, instead of to FxSound, are not enhanced and ignore the Windows volume and mute.");
	toggle_.setTooltip(tooltip);

	repaint();
}

String FxOutputsGrid::Tile::getSummary() const
{
	if (!output_.present)
		return TRANS("Disconnected");
	// BetterFxSound: see FxController::OutputState::failed.
	if (output_.failed)
		return TRANS("Could not be opened");

	return String(output_.volume_percent) + " %" + getSeparator()
		+ String(output_.delay_ms) + " ms" + getSeparator()
		+ getChannelModeText(output_.channel_mode);
}

void FxOutputsGrid::Tile::resized()
{
	auto bounds = getLocalBounds();
	auto gear_bounds = bounds.removeFromRight(GEAR_SIZE + 8);
	gear_button_.setBounds(gear_bounds.withSizeKeepingCentre(GEAR_SIZE, GEAR_SIZE));
	// The toggle covers the name too, so a click anywhere on it enables or disables the output.
	toggle_.setBounds(bounds.withTrimmedLeft(4));
}

void FxOutputsGrid::Tile::paint(Graphics& g)
{
	auto bounds = getLocalBounds().toFloat().reduced(0.5f);
	auto hover = isMouseOver(true);
	auto corner = 6.0f;

	if (output_.enabled)
	{
		g.setColour(getThemeColour(FxColor::ComboBoxBackground));
		g.fillRoundedRectangle(bounds, corner);
		g.setColour(getThemeColour(FxColor::SelectedRowOutline, hover ? 1.0f : 0.8f));
		g.drawRoundedRectangle(bounds, corner, 1.2f);
	}
	else
	{
		g.setColour(getThemeColour(FxColor::ComboBoxBackground, 0.35f));
		g.fillRoundedRectangle(bounds, corner);
		g.setColour(getThemeColour(FxColor::RowOutline, hover ? 0.5f : 0.2f));
		g.drawRoundedRectangle(bounds, corner, 1.0f);
	}

	auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel());
	if (theme == nullptr)
		return;

	auto alpha = output_.present ? 1.0f : 0.55f;
	auto text_bounds = getLocalBounds().withTrimmedLeft(4 + TICK_AREA).withTrimmedRight(GEAR_SIZE + 10).reduced(0, 4);
	auto name_bounds = text_bounds.removeFromTop(text_bounds.getHeight() / 2 + 2);

	g.setColour(output_.enabled ? getThemeColour(FxColor::HighlightedText, alpha) : getThemeColour(FxColor::DefaultText, alpha));
	g.setFont(theme->getNormalFont().withHeight(14.0f));
	g.drawFittedText(output_.name, name_bounds, Justification::bottomLeft, 1, 1.0f);

	if (output_.present && !output_.failed)
		g.setColour(getThemeColour(FxColor::DefaultText, output_.enabled ? 0.85f : 0.6f));
	else
		g.setColour(getThemeColour(FxColor::InvalidTextBorder));
	g.setFont(theme->getSmallFont().withHeight(12.0f));
	g.drawFittedText(getSummary(), text_bounds, Justification::topLeft, 1, 1.0f);
}

void FxOutputsGrid::Tile::lookAndFeelChanged()
{
	applyTheme();
}

void FxOutputsGrid::Tile::applyTheme()
{
	toggle_.setColour(ToggleButton::ColourIds::tickColourId, getThemeColour(FxColor::HighlightedText));
	toggle_.setColour(ToggleButton::ColourIds::tickDisabledColourId, getThemeColour(FxColor::DefaultText, 0.8f));

	// settings.svg is drawn in #7E7E7E, the hover image uses the highlighted text colour instead.
	auto svg = String::fromUTF8(BinaryData::settings_svg, BinaryData::settings_svgSize);
	auto hover_svg = svg.replace("#7E7E7E", "#" + getThemeColour(FxColor::HighlightedText).toDisplayString(false), true);
	gear_image_ = Drawable::createFromImageData(BinaryData::settings_svg, BinaryData::settings_svgSize);
	gear_hover_image_ = Drawable::createFromImageData(hover_svg.toRawUTF8(), hover_svg.getNumBytesAsUTF8());
	gear_button_.setImages(gear_image_.get(), gear_hover_image_.get());

	repaint();
}

void FxOutputsGrid::Tile::mouseEnter(const MouseEvent&)
{
	repaint();
}

void FxOutputsGrid::Tile::mouseExit(const MouseEvent&)
{
	repaint();
}

//==============================================================================
// FxOutputsGrid
//==============================================================================
FxOutputsGrid::FxOutputsGrid()
{
	setFocusContainerType(FocusContainerType::keyboardFocusContainer);
	setTitle(TRANS("Outputs"));

	viewport_.setViewedComponent(&container_, false);
	viewport_.setScrollBarsShown(true, false);
	addAndMakeVisible(viewport_);

	// No refresh here: the views are built before FxController::init, the first
	// OutputListUpdated fills the grid.
	FxModel::getModel().addListener(this);
}

FxOutputsGrid::~FxOutputsGrid()
{
	FxModel::getModel().removeListener(this);
}

String FxOutputsGrid::getMutedNotice()
{
	return TRANS("No output enabled") + String(CharPointer_UTF8(" \xe2\x80\x94 ")) + TRANS("sound is muted");
}

String FxOutputsGrid::getDisconnectedNotice()
{
	return TRANS("Enabled outputs are disconnected") + String(CharPointer_UTF8(" \xe2\x80\x94 ")) + TRANS("sound is muted");
}

String FxOutputsGrid::getFailedNotice(int failed)
{
	if (failed == 1)
		return TRANS("An output could not be opened. Another app may be using it exclusively.");
	return TRANS("%1 outputs could not be opened. Another app may be using them exclusively.").replace("%1", String(failed));
}

String FxOutputsGrid::getSilenceNotice(const Counts& counts)
{
	if (counts.enabled == 0)
		return getMutedNotice();
	// BetterFxSound: connected, but none of them could be opened.
	if (counts.playing == 0 && counts.failed > 0)
		return getFailedNotice(counts.failed);
	if (counts.playing == 0)
		return getDisconnectedNotice();
	return {};
}

FxOutputsGrid::Counts FxOutputsGrid::countOutputs(const std::vector<FxController::OutputState>& outputs)
{
	Counts counts;
	for (auto& output : outputs)
	{
		counts.total++;
		if (output.enabled)
		{
			counts.enabled++;
			if (output.present && output.failed)
				counts.failed++;
			else if (output.present)
				counts.playing++;
		}
	}
	return counts;
}

FxOutputsGrid::Counts FxOutputsGrid::getCounts() const
{
	std::vector<FxController::OutputState> outputs;
	for (auto& tile : tiles_)
		outputs.push_back(tile->getOutput());
	return countOutputs(outputs);
}

String FxOutputsGrid::getChannelModeText(OutputChannelMode channel_mode)
{
	switch (channel_mode)
	{
	case OutputChannelMode::Left:
		return TRANS("Left only");
	case OutputChannelMode::Right:
		return TRANS("Right only");
	case OutputChannelMode::Mono:
		return TRANS("Mono");
	case OutputChannelMode::Both:
	default:
		return TRANS("Both channels");
	}
}

void FxOutputsGrid::setColumns(int columns)
{
	columns_ = jmax(1, columns);
	layoutTiles();
}

void FxOutputsGrid::setMaxVisibleRows(int rows)
{
	max_visible_rows_ = jmax(1, rows);
}

int FxOutputsGrid::getRowCount() const
{
	auto count = static_cast<int>(tiles_.size());
	return jmax(1, (count + columns_ - 1) / columns_);
}

int FxOutputsGrid::getPreferredHeight() const
{
	auto rows = jmin(getRowCount(), max_visible_rows_);
	return rows * TILE_HEIGHT + (rows - 1) * TILE_GAP;
}

int FxOutputsGrid::getOutputCount() const
{
	return static_cast<int>(tiles_.size());
}

void FxOutputsGrid::refresh()
{
	auto outputs = FxController::getInstance().getOutputs();

	// Same outputs in the same order: update the tiles in place so keyboard focus and hover stay.
	bool same_outputs = (outputs.size() == tiles_.size());
	for (size_t i = 0; same_outputs && i < outputs.size(); i++)
	{
		same_outputs = (tiles_[i]->getOutput().id == outputs[i].id);
	}

	if (same_outputs)
	{
		for (size_t i = 0; i < outputs.size(); i++)
		{
			tiles_[i]->setOutput(outputs[i]);
		}
	}
	else
	{
		tiles_.clear();
		for (auto& output : outputs)
		{
			auto tile = std::make_unique<Tile>();
			tile->setOutput(output);
			tile->onEnabledChanged = [this](const FxController::OutputState& state) {
				FxController::getInstance().setOutputEnabled(state.id, state.enabled);
				if (onContentChanged)
					onContentChanged();
			};
			tile->onGearClicked = [this](Tile& clicked_tile) { showSettings(clicked_tile); };
			container_.addAndMakeVisible(tile.get());
			tiles_.push_back(std::move(tile));
		}
		layoutTiles();
	}

	repaint();

	if (onContentChanged)
		onContentChanged();
}

void FxOutputsGrid::showSettings(Tile& tile)
{
	Component::SafePointer<FxOutputsGrid> safe_this(this);
	FxOutputSettingsPanel::show(tile.getOutput(), tile.getGearButton(), [safe_this](const FxController::OutputState& state) {
		if (safe_this != nullptr)
			safe_this->updateOutputLocally(state);
	});
}

void FxOutputsGrid::updateOutputLocally(const FxController::OutputState& output)
{
	for (auto& tile : tiles_)
	{
		if (tile->getOutput().id == output.id)
		{
			auto state = tile->getOutput();
			state.volume_percent = output.volume_percent;
			state.delay_ms = output.delay_ms;
			state.channel_mode = output.channel_mode;
			tile->setOutput(state);
		}
	}
}

void FxOutputsGrid::layoutTiles()
{
	viewport_.setBounds(getLocalBounds());

	auto rows = static_cast<int>(tiles_.empty() ? 0 : getRowCount());
	auto content_height = rows > 0 ? rows * TILE_HEIGHT + (rows - 1) * TILE_GAP : 0;

	auto width = getWidth();
	if (content_height > getHeight())
		width -= viewport_.getScrollBarThickness() + 4;

	auto tile_width = jmax(0, (width - (columns_ - 1) * TILE_GAP) / columns_);
	container_.setSize(jmax(0, width), content_height);

	for (size_t i = 0; i < tiles_.size(); i++)
	{
		auto column = static_cast<int>(i) % columns_;
		auto row = static_cast<int>(i) / columns_;
		tiles_[i]->setBounds(column * (tile_width + TILE_GAP), row * (TILE_HEIGHT + TILE_GAP), tile_width, TILE_HEIGHT);
	}
}

void FxOutputsGrid::resized()
{
	layoutTiles();
}

void FxOutputsGrid::paint(Graphics& g)
{
	if (!tiles_.empty())
		return;

	auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel());
	if (theme != nullptr)
		g.setFont(theme->getSmallFont().withHeight(13.0f));
	g.setColour(getThemeColour(FxColor::DefaultText, 0.7f));
	g.drawFittedText(TRANS("No playback devices were found."), getLocalBounds().withHeight(TILE_HEIGHT), Justification::centredLeft, 1);
}

void FxOutputsGrid::modelChanged(FxModel::Event model_event)
{
	if (model_event == FxModel::Event::OutputListUpdated)
	{
		refresh();
	}
}

//==============================================================================
// FxOutputsPopup
//==============================================================================
FxOutputsPopup::FxOutputsPopup() : tooltip_window_(this)
{
	title_.setText(TRANS("Outputs"), NotificationType::dontSendNotification);
	title_.setJustificationType(Justification::centredLeft);

	notice_.setJustificationType(Justification::centredLeft);
	notice_.setMinimumHorizontalScale(0.8f);

	grid_.setColumns(1);
	grid_.setMaxVisibleRows(MAX_VISIBLE_ROWS);
	grid_.onContentChanged = [this]() {
		updateNotice();
		updateSize();
	};

	addAndMakeVisible(title_);
	addAndMakeVisible(notice_);
	addAndMakeVisible(grid_);

	applyTheme();
	grid_.refresh();
	updateNotice();
	updateSize();
}

void FxOutputsPopup::updateNotice()
{
	auto silence_notice = FxOutputsGrid::getSilenceNotice(grid_.getCounts());
	if (silence_notice.isNotEmpty())
	{
		notice_.setText(silence_notice, NotificationType::dontSendNotification);
		notice_.setColour(Label::ColourIds::textColourId, getThemeColour(FxColor::InvalidTextBorder));
	}
	else
	{
		notice_.setText(TRANS("Tick every device that should play the sound."), NotificationType::dontSendNotification);
		notice_.setColour(Label::ColourIds::textColourId, getThemeColour(FxColor::DefaultText, 0.7f));
	}
}

void FxOutputsPopup::updateSize()
{
	auto height = MARGIN + TITLE_HEIGHT + NOTICE_HEIGHT + 6 + grid_.getPreferredHeight() + MARGIN;
	if (getWidth() != WIDTH || getHeight() != height)
		setSize(WIDTH, height);
	else
		resized();
}

void FxOutputsPopup::resized()
{
	auto bounds = getLocalBounds().reduced(MARGIN);
	title_.setBounds(bounds.removeFromTop(TITLE_HEIGHT));
	notice_.setBounds(bounds.removeFromTop(NOTICE_HEIGHT));
	bounds.removeFromTop(6);
	grid_.setBounds(bounds);
}

void FxOutputsPopup::paint(Graphics&)
{
	// The CallOutBox draws the background.
}

void FxOutputsPopup::lookAndFeelChanged()
{
	applyTheme();
	updateNotice();
}

void FxOutputsPopup::applyTheme()
{
	title_.setColour(Label::ColourIds::textColourId, getThemeColour(FxColor::HighlightedText));
	if (auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel()))
	{
		title_.setFont(theme->getNormalFont());
		notice_.setFont(theme->getSmallFont().withHeight(12.5f));
	}
}

//==============================================================================
// FxOutputsButton
//==============================================================================
FxOutputsButton::FxOutputsButton() : Button("outputsButton")
{
	setMouseCursor(MouseCursor::PointingHandCursor);
	setWantsKeyboardFocus(true);
	setTooltip(TRANS("Choose the devices that play the sound"));
	// Not updateText(): the controller is not initialized yet when the views are built.
	setButtonText(TRANS("Outputs"));
}

void FxOutputsButton::updateText()
{
	// Count what really plays: an enabled output that is disconnected is silent.
	auto counts = FxOutputsGrid::countOutputs(FxController::getInstance().getOutputs());
	muted_ = (counts.playing == 0);
	if (muted_)
		setButtonText(TRANS("Outputs: muted"));
	else
		setButtonText(TRANS("Outputs") + " (" + String(counts.playing) + ")");
	setTooltip(muted_ ? FxOutputsGrid::getSilenceNotice(counts) : TRANS("Choose the devices that play the sound"));
	repaint();
}

void FxOutputsButton::setError(bool error)
{
	error_ = error;
	repaint();
}

void FxOutputsButton::paintButton(Graphics& g, bool highlighted, bool)
{
	auto bounds = getLocalBounds().toFloat();
	auto corner = bounds.getHeight() / 5.0f;

	g.setColour(getThemeColour(FxColor::ComboBoxBackground));
	g.fillRoundedRectangle(bounds, corner);

	Colour outline;
	if (hasKeyboardFocus(true))
		outline = getThemeColour(FxColor::SliderHighlight, 0.2f);
	else if (error_ || muted_)
		outline = getThemeColour(FxColor::SliderTrack);
	else
		outline = getThemeColour(FxColor::ComboBoxBackground);
	g.setColour(outline);
	g.drawRoundedRectangle(bounds.reduced(0.5f), corner, 1.0f);

	// Same arrow and text placement as FxTheme::drawComboBox / positionComboBoxText.
	auto margin = getWidth() <= 150 ? 24 : 32;
	auto arrow_bounds = juce::Rectangle<float>(static_cast<float>(getWidth() - margin), 0.0f, 12.0f, bounds.getHeight());
	auto arrow = highlighted ? Drawable::createFromImageData(FXIMAGE(DropDownArrowHover), FXIMAGESIZE(DropDownArrowHover))
							 : Drawable::createFromImageData(FXIMAGE(DropDownArrow), FXIMAGESIZE(DropDownArrow));
	if (arrow != nullptr)
		arrow->drawWithin(g, arrow_bounds, RectanglePlacement::centred, 1.0f);

	if (auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel()))
		g.setFont(theme->getNormalFont());
	g.setColour(highlighted ? getThemeColour(FxColor::HighlightedText) : getThemeColour(FxColor::DefaultText));
	g.drawFittedText(getButtonText(), getLocalBounds().withLeft(10).withRight(getWidth() - 37), Justification::centredLeft, 1, 1.0f);
}

void FxOutputsButton::clicked()
{
	if (callout_ != nullptr)
		return;

	// Same as the old dropdown: look for devices Windows did not report yet before showing the list.
	FxController::getInstance().checkDeviceChanges();

	auto& callout = CallOutBox::launchAsynchronously(std::make_unique<FxOutputsPopup>(), getScreenBounds(), nullptr);
	callout_ = &callout;
}
