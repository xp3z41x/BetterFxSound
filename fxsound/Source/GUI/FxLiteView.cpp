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
#include "FxLiteView.h"
#include "FxController.h"

//==============================================================================
FxLiteView::FxLiteView() : tool_tip_(this)
{
	outputs_button_.setSize(LIST_WIDTH, LIST_HEIGHT);
	outputs_button_.setTitle(TRANS("Outputs"));
	outputs_button_.addMouseListener(this, true);
	addAndMakeVisible(outputs_button_);
	output_anchor_ = &outputs_button_;

	setOpaque(false);
	setSize(WIDTH, HEIGHT);
}

void FxLiteView::resized()
{
	RectanglePlacement placement(RectanglePlacement::xMid | RectanglePlacement::doNotResize);

	auto bounds = getLocalBounds();
	auto component_bounds = preset_list_.getLocalBounds();

	component_bounds.setX(PRESET_LIST_X);
	component_bounds.setY(LIST_Y);
	preset_list_.setBounds(component_bounds);

	component_bounds = outputs_button_.getLocalBounds();
	component_bounds.setX(OUTPUT_LIST_X);
	component_bounds.setY(LIST_Y);
	outputs_button_.setBounds(component_bounds);
}

void FxLiteView::outputsChanged()
{
	outputs_button_.updateText();
	outputs_button_.setError(output_error_);
}

void FxLiteView::mouseEnter(const MouseEvent& mouse_event)
{
	FxView::mouseEnter(mouse_event);
}

void FxLiteView::mouseExit(const MouseEvent& mouse_event)
{
	// Re-evaluates the error notification once the mouse left the outputs button.
	FxView::mouseEnter(mouse_event);
}

void FxLiteView::paint(Graphics& g)
{
	auto& theme = dynamic_cast<LookAndFeel_V4&>(getLookAndFeel());

	g.setFillType(FillType(theme.getCurrentColourScheme().getUIColour(LookAndFeel_V4::ColourScheme::windowBackground)));
	g.fillAll();

	g.setFillType(FillType(Colour(FXCOLOR(DefaultFill)).withAlpha(0.2f)));
	g.fillRoundedRectangle(20, 22, BACKGROUND_WIDTH, BACKGROUND_HEIGHT, 10);

    auto power_state = FxModel::getModel().getPowerState();
    preset_list_.setEnabled(power_state);
}