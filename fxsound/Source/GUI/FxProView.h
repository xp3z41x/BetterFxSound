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

#pragma once

#include <JuceHeader.h>
#include "FxView.h"
#include "FxAudioControls.h"
#include "FxEqualizer.h"
#include "FxVisualizer.h"
#include "FxOutputsGrid.h"

//==============================================================================
/*
*/
class FxProView  : public FxView, private Timer
{
public:
    FxProView();
    ~FxProView();

	void startVisualizer();
	void pauseVisualizer();

	void update();

    void setLookAndFeel();

private:
	// BetterFxSound outputs rework: the output dropdown is replaced by an outputs header in the
	// top right (title, notice, "Sync delays with microphone") and a grid of output tiles under
	// the preset row. The height grows with the grid: one row of tiles for up to 4 outputs, two
	// rows for 5 to 8, and the grid scrolls when there are more.
	static constexpr int WIDTH = 1040;
	static constexpr int PRESET_LIST_X = 40;
	static constexpr int OUTPUTS_HEADER_X = 530;
	static constexpr int LIST_Y = 32;
	static constexpr int AUDIO_X = 40;
	static constexpr int LIST_WIDTH = 470;
	static constexpr int LIST_HEIGHT = 40;
	static constexpr int GRID_X = 40;
	static constexpr int GRID_WIDTH = 960;
	static constexpr int GRID_COLUMNS = 4;
	static constexpr int GRID_MAX_VISIBLE_ROWS = 2;
	static constexpr int SECTION_GAP = 16;
	static constexpr int SYNC_BUTTON_WIDTH = 220;
	static constexpr int SYNC_BUTTON_HEIGHT = 28;
	static constexpr int CALIBRATION_POLL_MS = 200;

	int getPreferredHeight();
	void updateHeight();
	void outputsChanged() override;
	void updateOutputsNotice();
	void onSyncClicked();
	void timerCallback() override;

	void resized() override;
	void paint(Graphics& g) override;
	void lookAndFeelChanged() override;
	void comboBoxChanged(ComboBox* combobox) override;
	void modelChanged(FxModel::Event model_event) override;

	void mouseEnter(const MouseEvent& mouse_event) override;
	void mouseExit(const MouseEvent& mouse_event) override;

	Component outputs_header_;
	Label outputs_title_;
	Label outputs_notice_;
	TextButton sync_button_;
	String sync_status_;
	FxOutputsGrid outputs_grid_;

	FxAudioControls audio_controls_;
	FxEqualizer equalizer_;
    TooltipWindow tool_tip_;
    FxVisualizer visualizer_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FxProView)
};
