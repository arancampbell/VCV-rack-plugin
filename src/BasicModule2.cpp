#include "plugin.hpp"

// Forward-declare the module class so the scope can have a pointer to it.
struct BasicModule2;

// Simple oscilloscope display widget that now reads zoom from the module.
struct SimpleScope : TransparentWidget {
    // A pointer to the module to access its parameters.
    BasicModule2* module = nullptr;

    // A buffer to store the waveform data.
    static const int BUFFER_SIZE = 2048;
    float buffer[BUFFER_SIZE] = {};
    int writeIndex = 0; // The position where the next sample will be written.

    SimpleScope() {
        std::memset(buffer, 0, sizeof(buffer));
    }

    // A simple ring-buffer write. No triggering logic.
    void addSample(float sample) {
        buffer[writeIndex] = sample;
        writeIndex = (writeIndex + 1) % BUFFER_SIZE;
    }

    // We only DECLARE drawLayer here. The full definition is moved after the
    // BasicModule2 struct is fully defined, to resolve the incomplete type error.
    void drawLayer(const DrawArgs& args, int layer) override;
};


struct BasicModule2 : Module {
    enum ParamId {
        PITCH_PARAM,
        ZOOM_PARAM, // New parameter for oscilloscope zoom
        PARAMS_LEN
    };
    enum InputId {
        PITCH_INPUT,
        INPUTS_LEN
    };
    enum OutputId {
        SINE_OUTPUT,
        OUTPUTS_LEN
    };
    enum LightId {
        BLINK_LIGHT,
        LIGHTS_LEN
    };

    float phase = 0.f;
    float blinkPhase = 0.f;

    // Oscilloscope
    SimpleScope* scope = nullptr;

    // Re-introduce downsampling to control the scrolling speed of the waveform.
    int scopeSampleCount = 0;
    static const int SCOPE_DOWNSAMPLE = 8; // Only send every 8th sample to the scope.

    BasicModule2() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(PITCH_PARAM, 0.f, 1.f, 0.f, "Pitch");
        // Configure the new zoom parameter
        configParam(ZOOM_PARAM, 0.f, 1.f, 0.5f, "Zoom");
        configInput(PITCH_INPUT, "1V/Oct");
        configOutput(SINE_OUTPUT, "Audio");
    }

    void process(const ProcessArgs& args) override {
        // Get pitch from knob and input
        float pitch = params[PITCH_PARAM].getValue();
        pitch += inputs[PITCH_INPUT].getVoltage();
        // The default frequency is C4 = 261.6256f
        float freq = dsp::FREQ_C4 * std::pow(2.f, pitch);

        // Accumulate the phase
        phase += freq * args.sampleTime;
        if (phase >= 1.f)
            phase -= 1.f;

        // Compute the sine output
        float sine = std::sin(2.f * M_PI * phase);
        // Audio signals are typically +/-5V
        float output = 5.f * sine;
        outputs[SINE_OUTPUT].setVoltage(output);

        // Send data to oscilloscope (downsampled to control scroll speed)
        if (scope) {
            scopeSampleCount++;
            if (scopeSampleCount >= SCOPE_DOWNSAMPLE) {
                scope->addSample(output);
                scopeSampleCount = 0;
            }
        }

        // Blink light at 1Hz
        blinkPhase += args.sampleTime;
        if (blinkPhase >= 1.f)
            blinkPhase -= 1.f;
        lights[BLINK_LIGHT].setBrightness(blinkPhase < 0.5f ? 1.f : 0.f);
    }
};

// Now we define the implementation of drawLayer, after BasicModule2 is fully known.
void SimpleScope::drawLayer(const DrawArgs& args, int layer) {
    if (layer != 1) {
        TransparentWidget::drawLayer(args, layer);
        return;
    }

    // Draw background
    nvgBeginPath(args.vg);
    nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
    nvgFillColor(args.vg, nvgRGBA(0, 0, 0, 200));
    nvgFill(args.vg);

    // Draw grid
    nvgStrokeColor(args.vg, nvgRGBA(50, 50, 50, 255));
    nvgStrokeWidth(args.vg, 1.0);

    // Horizontal center line
    nvgBeginPath(args.vg);
    nvgMoveTo(args.vg, 0, box.size.y / 2);
    nvgLineTo(args.vg, box.size.x, box.size.y / 2);
    nvgStroke(args.vg);

    // Vertical grid lines
    for (int i = 1; i < 4; i++) {
        nvgBeginPath(args.vg);
        float x = (box.size.x / 4) * i;
        nvgMoveTo(args.vg, x, 0);
        nvgLineTo(args.vg, x, box.size.y);
        nvgStroke(args.vg);
    }

    // --- Waveform Drawing Logic ---

    int samplesToDisplay = 256; // Default value in case module isn't set.
    if (module) {
        // Read the zoom knob's value (0.0 to 1.0).
        float zoomValue = module->params[BasicModule2::ZOOM_PARAM].getValue();
        // Map the knob's value to a number of samples to display.
        // A higher zoom value means more zoom, so we display fewer samples.
        // This creates a range from 2048 samples (zoomed out) to 32 (zoomed in).
        samplesToDisplay = (int)rack::math::rescale(zoomValue, 0.f, 1.f, 2048.f, 32.f);
    }

    nvgBeginPath(args.vg);
    nvgStrokeColor(args.vg, nvgRGBA(255, 255, 0, 255));
    nvgStrokeWidth(args.vg, 1.5);

    // Determine the starting point for reading from the buffer
    int startReadIndex = (writeIndex - samplesToDisplay + BUFFER_SIZE) % BUFFER_SIZE;

    for (int i = 0; i < samplesToDisplay; i++) {
        int idx = (startReadIndex + i) % BUFFER_SIZE;

        // Scale X to fit the window, stretching the samples across the full width.
        float x = (float)i / (samplesToDisplay - 1) * box.size.x;

        // Map -5V to +5V to screen space
        float y = box.size.y - ((buffer[idx] + 5.f) / 10.f) * box.size.y;

        if (i == 0) {
            nvgMoveTo(args.vg, x, y);
        } else {
            nvgLineTo(args.vg, x, y);
        }
    }
    nvgStroke(args.vg);

    TransparentWidget::drawLayer(args, layer);
}


struct BasicModule2Widget : ModuleWidget {
    BasicModule2Widget(BasicModule2* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/BasicModule2.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(15.24, 46.063)), module, BasicModule2::PITCH_PARAM));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(15.24, 77.478)), module, BasicModule2::PITCH_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(15.24, 108.713)), module, BasicModule2::SINE_OUTPUT));

        addChild(createLightCentered<MediumLight<RedLight>>(mm2px(Vec(15.24, 30.224)), module, BasicModule2::BLINK_LIGHT));

        // Add the new zoom knob with the correct coordinates from your SVG.
        addParam(createParamCentered<RoundBlackKnob>(mm2px(Vec(63.081, 89.628)), module, BasicModule2::ZOOM_PARAM));

        // Add oscilloscope display
        if (module) {
            SimpleScope* scope = new SimpleScope();
            scope->box.pos = mm2px(Vec(35.0, 38.0));
            scope->box.size = mm2px(Vec(50, 40));
            // Give the scope a pointer to the module so it can read the zoom knob.
            scope->module = module;
            addChild(scope);
            module->scope = scope;
        }
    }
};

// Register the module with VCV Rack
Model* modelBasicModule2 = createModel<BasicModule2, BasicModule2Widget>("BasicModule2");

