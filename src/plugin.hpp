#include "rack.hpp"

using namespace rack;

const NVGcolor BLUE_BIDOO = nvgRGBA(42, 87, 117, 255);
const NVGcolor LIGHTBLUE_BIDOO = nvgRGBA(45, 114, 143, 255);
const NVGcolor RED_BIDOO = nvgRGBA(205, 31, 0, 255);
const NVGcolor YELLOW_BIDOO = nvgRGBA(255, 233, 0, 255);
const NVGcolor YELLOW_BIDOO_LIGHT = nvgRGBA(255, 233, 0, 25);
const NVGcolor SAND_BIDOO = nvgRGBA(230, 220, 191, 255);
const NVGcolor ORANGE_BIDOO = nvgRGBA(228, 87, 46, 255);
const NVGcolor PINK_BIDOO = nvgRGBA(164, 3, 111, 255);
const NVGcolor GREEN_BIDOO = nvgRGBA(2, 195, 154, 255);

extern Plugin *pluginInstance;

extern Model *modelLIMONADE;
extern Model* modelBasicModule;
extern Model* modelBasicModule2;

struct InstantiateExpanderItem : MenuItem {
	Module* module;
	Model* model;
	Vec posit;
	void onAction(const event::Action &e) override;
};

struct BidooModule : Module {
	int themeId = -1;
	bool themeChanged = true;
	bool loadDefault = true;
	json_t *dataToJson() override;
	void dataFromJson(json_t *rootJ) override;
};

struct BidooWidget : ModuleWidget {
	SvgPanel* lightPanel;
	SvgPanel* darkPanel;
	SvgPanel* blackPanel;
	SvgPanel* bluePanel;
	SvgPanel* greenPanel;
	int defaultPanelTheme = 0;

	BidooWidget() {
		readThemeAndContrastFromDefault();
	}

	struct LightItem : MenuItem {
		BidooModule *module;
		BidooWidget *pWidget;
		void onAction(const event::Action &e) override {
			module->themeId = 0;
			module->themeChanged = true;
			pWidget->defaultPanelTheme = 0;
			pWidget->writeThemeAndContrastAsDefault();
		}
	};

	struct DarkItem : MenuItem {
		BidooModule *module;
		BidooWidget *pWidget;
		void onAction(const event::Action &e) override {
			module->themeId = 1;
			module->themeChanged = true;
			pWidget->defaultPanelTheme = 1;
			pWidget->writeThemeAndContrastAsDefault();
		}
	};

	struct BlackItem : MenuItem {
		BidooModule *module;
		BidooWidget *pWidget;
		void onAction(const event::Action &e) override {
			module->themeId = 2;
			module->themeChanged = true;
			pWidget->defaultPanelTheme = 2;
			pWidget->writeThemeAndContrastAsDefault();
		}
	};

	struct BlueItem : MenuItem {
		BidooModule *module;
		BidooWidget *pWidget;
		void onAction(const event::Action &e) override {
			module->themeId = 3;
			module->themeChanged = true;
			pWidget->defaultPanelTheme = 3;
			pWidget->writeThemeAndContrastAsDefault();
		}
	};

	struct GreenItem : MenuItem {
		BidooModule *module;
		BidooWidget *pWidget;
		void onAction(const event::Action &e) override {
			module->themeId = 4;
			module->themeChanged = true;
			pWidget->defaultPanelTheme = 4;
			pWidget->writeThemeAndContrastAsDefault();
		}
	};

	void writeThemeAndContrastAsDefault();
	void readThemeAndContrastFromDefault();
	void prepareThemes(const std::string& filename);
	void appendContextMenu(Menu *menu) override;
	void step() override;
};
