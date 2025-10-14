#include "plugin.hpp"

// Simple oscilloscope display widget
struct SimpleScope : TransparentWidget {
    static const int BUFFER_SIZE = 512;
    float buffer[BUFFER_SIZE] = {};
    int bufferIndex = 0;

    void addSample(float sample) {
        buffer[bufferIndex] = sample;
        bufferIndex = (bufferIndex + 1) % BUFFER_SIZE;
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer != 1) return;

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

        // Draw waveform
        nvgBeginPath(args.vg);
        nvgStrokeColor(args.vg, nvgRGBA(0, 255, 100, 255));
        nvgStrokeWidth(args.vg, 1.5);

        for (int i = 0; i < BUFFER_SIZE; i++) {
            int idx = (bufferIndex + i) % BUFFER_SIZE;
            float x = (float)i / BUFFER_SIZE * box.size.x;
            // Map -5V to +5V to screen space
            float y = box.size.y / 2 - (buffer[idx] / 10.0f) * box.size.y;

            if (i == 0) {
                nvgMoveTo(args.vg, x, y);
            } else {
                nvgLineTo(args.vg, x, y);
            }
        }
        nvgStroke(args.vg);

        TransparentWidget::drawLayer(args, layer);
    }
};

struct BasicModule2 : Module {
    enum ParamId {
        PITCH_PARAM,
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
    int scopeSampleCount = 0;
    static const int SCOPE_DOWNSAMPLE = 8; // Only send every Nth sample to scope

    BasicModule2() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configParam(PITCH_PARAM, 0.f, 1.f, 0.f, "Pitch");
        configInput(PITCH_INPUT, "1V/Oct");
        configOutput(SINE_OUTPUT, "Audio");
    }

    void process(const ProcessArgs& args) override {
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
        // https://vcvrack.com/manual/VoltageStandards
        float output = 5.f * sine;
        outputs[SINE_OUTPUT].setVoltage(output);

        // Send to oscilloscope (downsampled)
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

        // Add oscilloscope display
        // mm2px(Vec(41.16, 24.856)) was the commented reference point
        if (module) {
            SimpleScope* scope = new SimpleScope();
            scope->box.pos = mm2px(Vec(43.565, 51.048));
            scope->box.size = mm2px(Vec(42, 25));
            addChild(scope);
            module->scope = scope;
        }
    }
};

Model* modelBasicModule2 = createModel<BasicModule2, BasicModule2Widget>("BasicModule2");