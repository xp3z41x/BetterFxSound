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

#include <JuceHeader.h>
#include "FxProView.h"
#include "FxController.h"
#include "FxTheme.h"
#include "FxWindow.h"

//==============================================================================
FxProView::FxProView() : tool_tip_(this)
{
	outputs_title_.setText(TRANS("Outputs"), NotificationType::dontSendNotification);
	outputs_title_.setJustificationType(Justification::centredLeft);
	outputs_title_.setInterceptsMouseClicks(false, false);

	outputs_notice_.setJustificationType(Justification::topLeft);
	outputs_notice_.setMinimumHorizontalScale(0.6f);

	sync_button_.setButtonText(TRANS("Sync delays with microphone"));
	sync_button_.setMouseCursor(MouseCursor::PointingHandCursor);
	sync_button_.setWantsKeyboardFocus(true);
	sync_button_.setTooltip(TRANS("Plays beeps on each enabled output and sets the delays so they play in time"));
	sync_button_.onClick = [this]() { onSyncClicked(); };

	outputs_header_.addAndMakeVisible(outputs_title_);
	outputs_header_.addAndMakeVisible(outputs_notice_);
	outputs_header_.addAndMakeVisible(sync_button_);
	outputs_header_.addMouseListener(this, true);
	addAndMakeVisible(outputs_header_);
	output_anchor_ = &outputs_header_;

	outputs_grid_.setColumns(GRID_COLUMNS);
	outputs_grid_.setMaxVisibleRows(GRID_MAX_VISIBLE_ROWS);
	outputs_grid_.onContentChanged = [this]() { outputsChanged(); };
	addAndMakeVisible(outputs_grid_);

	addAndMakeVisible(audio_controls_);
	addAndMakeVisible(equalizer_);
    addChildComponent(visualizer_);

	equalizer_.addMouseListener(this, true);
	audio_controls_.addMouseListener(this, true);

    auto& theme = dynamic_cast<LookAndFeel_V4&>(getLookAndFeel());

    tool_tip_.setColour(TooltipWindow::ColourIds::textColourId, theme.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::defaultText));
    tool_tip_.setOpaque(false);

	setOpaque(false);
	// The notice is filled on the first OutputListUpdated: the controller is not initialized
	// yet when the views are built.
	setSize(WIDTH, getPreferredHeight());
}

FxProView::~FxProView()
{
	// Closing the window while measuring stops the measurement.
	if (isTimerRunning())
	{
		stopTimer();
		FxController::getInstance().cancelDelayCalibration();
	}
}

void FxProView::startVisualizer()
{
	visualizer_.start();
}

void FxProView::pauseVisualizer()
{
	visualizer_.pause();
}

void FxProView::update()
{
	audio_controls_.update();
	equalizer_.update();

	visualizer_.calcGradient();

    if (FxController::getInstance().isAudioProcessing())
    {
        visualizer_.reset();
    }

	visualizer_.setVisible(true);
	setSize(WIDTH, getPreferredHeight());
	// -------------------------- Values always visible since version 2.0 (otherwise they do not appear on touch screens)
	equalizer_.showValues(true);
	audio_controls_.showValues(true);
}

void FxProView::setLookAndFeel()
{
	audio_controls_.setLookAndFeel();
	updateOutputsNotice();
}

int FxProView::getPreferredHeight()
{
	auto y = LIST_Y + LIST_HEIGHT + 12 + outputs_grid_.getPreferredHeight() + SECTION_GAP;
	if (visualizer_.isVisible())
	{
		y += visualizer_.getHeight() + SECTION_GAP;
	}
	// Audio controls, the bottom padding of the panel and the margin under it.
	return y + audio_controls_.getHeight() + 18 + 8;
}

void FxProView::updateHeight()
{
	auto height = getPreferredHeight();
	if (height == getHeight())
	{
		resized();
		return;
	}

	setSize(WIDTH, height);

	// The window takes its size from its content when the content is set, so set it again.
	if (auto* window = dynamic_cast<FxWindow*>(getParentComponent()))
	{
		window->setContent(this);
	}
}

void FxProView::outputsChanged()
{
	updateOutputsNotice();
	updateHeight();
}

void FxProView::updateOutputsNotice()
{
	auto& controller = FxController::getInstance();
	auto enabled_count = controller.getEnabledOutputCount();
	auto output_count = outputs_grid_.getOutputCount();

	auto error_colour = Colour(FXCOLOR(InvalidTextBorder)).withAlpha(1.0f);
	auto text_colour = Colour(FXCOLOR(DefaultText)).withAlpha(1.0f);

	String notice;
	Colour colour = text_colour;
	if (enabled_count == 0 && !isTimerRunning())
	{
		notice = FxOutputsGrid::getMutedNotice();
		colour = error_colour;
	}
	else if (output_error_)
	{
		notice = TRANS("FxSound can't play through one of the outputs. Point here for help.");
		colour = error_colour;
	}
	else if (sync_status_.isNotEmpty())
	{
		notice = sync_status_;
	}
	else
	{
		notice = TRANS("%1 of %2 outputs playing").replace("%1", String(enabled_count)).replace("%2", String(output_count));
		colour = text_colour.withAlpha(0.7f);
	}

	outputs_notice_.setText(notice, NotificationType::dontSendNotification);
	outputs_notice_.setTooltip(notice);
	outputs_notice_.setColour(Label::ColourIds::textColourId, colour);
	outputs_title_.setColour(Label::ColourIds::textColourId, Colour(FXCOLOR(HighlightedText)).withAlpha(1.0f));

	if (auto* theme = dynamic_cast<FxTheme*>(&getLookAndFeel()))
	{
		outputs_title_.setFont(theme->getNormalFont());
		outputs_notice_.setFont(theme->getSmallFont().withHeight(12.5f));
	}

	sync_button_.setButtonText(isTimerRunning() ? TRANS("Cancel sync") : TRANS("Sync delays with microphone"));
	sync_button_.setEnabled(isTimerRunning() || enabled_count > 0);
}

void FxProView::onSyncClicked()
{
	auto& controller = FxController::getInstance();

	if (isTimerRunning())
	{
		controller.cancelDelayCalibration();
		return;
	}

	if (!controller.startDelayCalibration())
	{
		sync_status_ = TRANS("Sync could not start. Make sure FxSound is on and at least one output is enabled.");
		updateOutputsNotice();
		return;
	}

	sync_status_ = TRANS("Measuring... Keep the room quiet, the beeps will play on each device in turn.");
	outputs_grid_.setEnabled(false);
	startTimer(CALIBRATION_POLL_MS);
	updateOutputsNotice();
}

void FxProView::timerCallback()
{
	auto& controller = FxController::getInstance();
	int progress = 0;
	auto state = controller.getDelayCalibrationState(&progress);

	if (state == 1)
	{
		sync_status_ = TRANS("Measuring... Keep the room quiet, the beeps will play on each device in turn.") + " " + String(progress) + "%";
		updateOutputsNotice();
		return;
	}

	stopTimer();
	outputs_grid_.setEnabled(true);

	if (state == 2)
	{
		sync_status_ = controller.applyDelayCalibrationResults();
		// The controller also sends OutputListUpdated, refresh now so the new delays show at once.
		outputs_grid_.refresh();
	}
	else if (state == 3)
	{
		sync_status_ = TRANS("Sync failed:") + " " + controller.getDelayCalibrationError();
	}
	else
	{
		sync_status_ = TRANS("Sync cancelled.");
	}

	updateOutputsNotice();
}

void FxProView::lookAndFeelChanged()
{
	FxView::lookAndFeelChanged();
	updateOutputsNotice();
}

void FxProView::resized()
{
    preset_list_.setBounds(PRESET_LIST_X, LIST_Y, LIST_WIDTH, LIST_HEIGHT);

	// Outputs header in the place of the old output dropdown: title and notice on the left,
	// sync button on the right.
	outputs_header_.setBounds(OUTPUTS_HEADER_X, LIST_Y - 4, LIST_WIDTH, LIST_HEIGHT + 8);
	auto header_bounds = outputs_header_.getLocalBounds();
	sync_button_.setBounds(header_bounds.removeFromRight(SYNC_BUTTON_WIDTH).withSizeKeepingCentre(SYNC_BUTTON_WIDTH, SYNC_BUTTON_HEIGHT));
	header_bounds.removeFromRight(10);
	outputs_title_.setBounds(header_bounds.removeFromTop(20));
	outputs_notice_.setBounds(header_bounds);

	auto grid_y = LIST_Y + LIST_HEIGHT + 12;
	outputs_grid_.setBounds(GRID_X, grid_y, GRID_WIDTH, outputs_grid_.getPreferredHeight());

	auto y = outputs_grid_.getBottom() + SECTION_GAP;
    if (visualizer_.isVisible())
    {
        visualizer_.setBounds(40, y, visualizer_.getWidth(), visualizer_.getHeight());
        y = visualizer_.getBottom() + SECTION_GAP;
    }

	audio_controls_.setBounds(audio_controls_.getBounds().withX(AUDIO_X).withY(y));
	equalizer_.setBounds(equalizer_.getBounds().withX(audio_controls_.getRight() + 16).withY(y));
}

void FxProView::paint(Graphics& g)
{
	auto& theme = dynamic_cast<LookAndFeel_V4&>(getLookAndFeel());

	g.setFillType(FillType(theme.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::windowBackground)));
	g.fillAll();

	auto panel_bottom = audio_controls_.getBottom() + 18;
	g.setFillType(FillType(Colour(FXCOLOR(PanelBackground)).withAlpha(0.2f)));
	g.fillRoundedRectangle(20.0f, 16.0f, 1000.0f, static_cast<float>(panel_bottom - 16), 8.0f);

    auto enable_controls = FxModel::getModel().getPowerState();

    preset_list_.setEnabled(enable_controls);
    audio_controls_.setEnabled(enable_controls);
    equalizer_.setEnabled(enable_controls);
	visualizer_.setEnabled(enable_controls);
}

void FxProView::comboBoxChanged(ComboBox* combobox)
{
	FxView::comboBoxChanged(combobox);
}

void FxProView::modelChanged(FxModel::Event model_event)
{
	FxView::modelChanged(model_event);

	if (model_event == FxModel::Event::PresetSelected)
	{
		update();
	}
}

void FxProView::mouseEnter(const MouseEvent& mouse_event)
{
	FxView::mouseEnter(mouse_event);
}

void FxProView::mouseExit(const MouseEvent& mouse_event)
{
	FxView::mouseEnter(mouse_event);
}
