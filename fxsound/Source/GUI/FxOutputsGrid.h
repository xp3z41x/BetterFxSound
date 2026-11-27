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
#include "FxController.h"
#include "FxModel.h"

// BetterFxSound outputs rework (docs/OUTPUTS_REWORK.md): the outputs UI shared by the Pro window,
// the Lite window popup and nothing else. Every output is a tile with a checkbox, its name and a
// gear button that opens the fine controls (volume, delay, channels) in a callout.

//==============================================================================
// Fine controls of one output. Shown in a CallOutBox from the gear button of a tile.
// Every change is applied right away through FxController.
class FxOutputSettingsPanel : public Component
{
public:
	static constexpr int WIDTH = 330;
	static constexpr int HEIGHT = 150;

	explicit FxOutputSettingsPanel(const FxController::OutputState& output);
	~FxOutputSettingsPanel() override;

	// Opens the panel in a callout pointing at target. on_changed is called with the new state
	// after each change, so the caller can update its summary without waiting for a model event
	// (volume, delay and channel changes do not send one).
	static void show(const FxController::OutputState& output, Component& target,
					 std::function<void(const FxController::OutputState&)> on_changed);

	std::function<void(const FxController::OutputState&)> onChanged;

	void resized() override;
	void paint(Graphics& g) override;
	void lookAndFeelChanged() override;

private:
	static constexpr int MARGIN = 14;
	static constexpr int ROW_HEIGHT = 30;
	static constexpr int LABEL_WIDTH = 74;

	void applyTheme();
	void saveDelay();
	void notifyChanged();
	static void setupSlider(Slider& slider, double max_value, double interval, const String& suffix);

	FxController::OutputState output_;
	Label title_;
	Label volume_label_;
	Slider volume_slider_;
	Label delay_label_;
	Slider delay_slider_;
	Label channel_label_;
	ComboBox channel_list_;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxOutputSettingsPanel)
};

//==============================================================================
// Grid of output tiles, one per entry of FxController::getOutputs(). Refreshes itself on
// FxModel::Event::OutputListUpdated. Scrolls vertically when there are more rows than
// setMaxVisibleRows allows.
class FxOutputsGrid : public Component, public FxModel::Listener
{
public:
	static constexpr int TILE_HEIGHT = 42;
	static constexpr int TILE_GAP = 8;

	FxOutputsGrid();
	~FxOutputsGrid() override;

	void setColumns(int columns);
	void setMaxVisibleRows(int rows);

	// Rows needed to show every tile, at least 1 (the empty message takes one row).
	int getRowCount() const;
	// Height that shows min(getRowCount(), max visible rows) rows.
	int getPreferredHeight() const;
	int getOutputCount() const;

	// Reads the outputs from the controller again.
	void refresh();

	// Called after a refresh or after the user enabled/disabled an output from a tile.
	std::function<void()> onContentChanged;

	void resized() override;
	void paint(Graphics& g) override;
	void modelChanged(FxModel::Event model_event) override;

	// Short texts shared by the views that show the grid.
	static String getMutedNotice();
	static String getChannelModeText(OutputChannelMode channel_mode);

private:
	class Tile : public Component
	{
	public:
		Tile();
		~Tile() override = default;

		void setOutput(const FxController::OutputState& output);
		const FxController::OutputState& getOutput() const { return output_; }

		std::function<void(const FxController::OutputState&)> onEnabledChanged;
		std::function<void(Tile&)> onGearClicked;
		Component& getGearButton() { return gear_button_; }

		void resized() override;
		void paint(Graphics& g) override;
		void lookAndFeelChanged() override;
		void mouseEnter(const MouseEvent& mouse_event) override;
		void mouseExit(const MouseEvent& mouse_event) override;

	private:
		static constexpr int GEAR_SIZE = 26;
		static constexpr int TICK_AREA = 30;

		void applyTheme();
		String getSummary() const;

		FxController::OutputState output_;
		ToggleButton toggle_;
		DrawableButton gear_button_;
		std::unique_ptr<Drawable> gear_image_;
		std::unique_ptr<Drawable> gear_hover_image_;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Tile)
	};

	void layoutTiles();
	void showSettings(Tile& tile);
	void updateOutputLocally(const FxController::OutputState& output);

	int columns_ = 4;
	int max_visible_rows_ = 2;
	Viewport viewport_;
	Component container_;
	std::vector<std::unique_ptr<Tile>> tiles_;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxOutputsGrid)
};

//==============================================================================
// Content of the Lite window's outputs popup: a title, the muted notice and a one-column grid.
class FxOutputsPopup : public Component
{
public:
	static constexpr int WIDTH = 340;

	FxOutputsPopup();
	~FxOutputsPopup() override = default;

	void resized() override;
	void paint(Graphics& g) override;
	void lookAndFeelChanged() override;

private:
	static constexpr int MARGIN = 12;
	static constexpr int TITLE_HEIGHT = 22;
	static constexpr int NOTICE_HEIGHT = 20;
	static constexpr int MAX_VISIBLE_ROWS = 6;

	void updateNotice();
	void updateSize();
	void applyTheme();

	Label title_;
	Label notice_;
	FxOutputsGrid grid_;
	TooltipWindow tooltip_window_;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxOutputsPopup)
};

//==============================================================================
// Button with the look of FxComboBox that shows "Outputs (N)" and opens FxOutputsPopup.
// Used by the Lite window in place of the old output dropdown.
class FxOutputsButton : public Button
{
public:
	FxOutputsButton();
	~FxOutputsButton() override = default;

	void updateText();
	void setError(bool error);
	bool getError() const { return error_; }

	void paintButton(Graphics& g, bool highlighted, bool down) override;
	void clicked() override;

private:
	bool error_ = false;
	bool muted_ = false;
	Component::SafePointer<CallOutBox> callout_;

	JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FxOutputsButton)
};
