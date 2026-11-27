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

#ifndef FXVIEW_H
#define FXVIEW_H

#include <JuceHeader.h>
#include "FxModel.h"
#include "FxController.h"
#include "FxNotification.h"
#include "FxComboBox.h"
#include "FxHyperlink.h"

//==============================================================================
/*
*/
class FxView : public Component, public ComboBox::Listener, public FxModel::Listener
{
public:
	FxView();
	~FxView();

protected:
	static constexpr int LIST_WIDTH = 225;
	static constexpr int LIST_HEIGHT = 50;
	
    void showErrorNotification(bool show);

	// Called on the message thread after the outputs or the output error state changed.
	virtual void outputsChanged() {}

	void comboBoxChanged(ComboBox* combobox) override;
	void modelChanged(FxModel::Event model_event) override;

	void mouseEnter(const MouseEvent&) override;
	void mouseExit(const MouseEvent&) override;

	void lookAndFeelChanged() override;

	void updatePresetButton();
	void setUndoButtonImages();

	DrawableButton undo_button_;
	std::unique_ptr<Drawable> undo_image_;
	std::unique_ptr<Drawable> undo_hover_image_;
	FxComboBox preset_list_;
    FxNotification error_notification_;

	// The outputs control the error notification points at (set by the derived views), and
	// whether FxSound can't play through its playback device (FxModel::Event::OutputError).
	Component* output_anchor_ = nullptr;
	bool output_error_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxView)
};

#endif